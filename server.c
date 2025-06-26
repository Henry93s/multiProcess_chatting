#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>       // 4단계: 시그널 처리용
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#define PORT    5101
#define PENDING_CONN 5
#define MAX_CLIENTS 30 // 최대 클라이언트 수 30
#define MAX_ROOMS 5 // 최대 채팅방 수 5

// chat-dev1 0단계(구조 변경 및 프로토콜 설계)
// chat-dev1 : 서버 측 데이터 구조 정의 - 클라이언트를 pid 가 아닌 닉네임, 현재 접속한 방 등의 정보로 관리할 구조체 정의
typedef struct {
    pid_t pid;
    int client_sock_fd; // 기존 client_sock 배열
    char nickName[50];
    int room_idx; // 현재 접속한 방 index (0 : lobby)
} ClientData;

// chat-dev1 : 채팅방 데이터 구조 정의
typedef struct {
    char roomName[100];
    int is_active; // 1 : 활성화, 0 : 비활성화
} RoomData;

// chat-dev1 : 서버 측 client 와 채팅방 데이터 구조 struct 전역 변수
ClientData clients[MAX_CLIENTS]; // 기존 child_pid, client_sock 배열 통합
RoomData rooms[MAX_ROOMS]; // 채팅방 배열

// 3 -> 4단계: 전역 변수로 pipe, conn_sock, child_pid 정의
int pipe_parent_to_child[MAX_CLIENTS][2]; // 부모 → 자식 write 기준으로 변수 이름 정의 
int pipe_child_to_parent[MAX_CLIENTS][2]; // 자식 → 부모 write 기준으로 변수 이름 정의

// chat-dev1 : 실제 루프를 돌 때 사용할 경계 값 추가
int active_client_count = 0;
int child_index = -1; // 자식 프로세스 전용 인덱스
// listen_fd : 서버가 클라이언트 연결을 기다리기 위한 소켓(서버 대기용) -> main() 함수 내 while(1) 내내 유지됨
// conn_fd : 클라이언트와 연결이 성공된 직후 사용되는 소켓 -> accept 성공 후 생성되고 자식에 넘기고 부모는 닫음
// 6 단계 : graceful_shutdown 을 위해 main 지역 변수에서 전역 변수로 이동
int listen_fd, conn_fd;
// 7 단계 : 서버 데몬화 처리 및 로그 출력을 파일로 리디렉션
int file_fd;

// 7단계 : 로그 출력을 위해서 시간을 [YYYY-MM-DD HH:MM:SS] 형식으로 문자열을 생성하는 함수
void get_timestamp(char* dest, size_t size, const char* msg) {
    time_t now = time(NULL);
    struct tm* t = localtime(&now);

    char timestamp[32];       // 시간 문자열 저장용

    strftime(timestamp, sizeof(timestamp), "[%Y-%m-%d %H:%M:%S] ", t);
    snprintf(dest, size, "%s%s", timestamp, msg);
}

// 4단계: SIGUSR1, SIGUSR2 핸들러 함수 
// 부모 시그널 핸들러 SIGUSR1 : 자식이 부모에게 메시지를 보냈음을 알리면 이를 부모가 읽음
// chat-dev1 : 메시지를 읽고 메시지 명령어에 해당하는 동작을 취하도록 함 -> 프로토콜 처리 허브 역할
void sigusr1_handler(int signo) {
    // 7단계 : LOG Redirection
    char logMsg[BUFSIZ * 2 + 32];
    char errMsg[BUFSIZ * 2];
    snprintf(errMsg, sizeof(errMsg), "[INFO] : [부모 pid %d] SIGUSR1 핸들러 발생", getpid()); // 로그 TYPE 문자열 결합
    get_timestamp(logMsg, sizeof(logMsg), errMsg);
    printf("\n%s", logMsg); // 로그에 현재 시간 + 관련 로그 출력
    fflush(stdout);

    char buf[BUFSIZ + 10 + 50];
    int n;
        
    // 4단계 -> chat-dev1 : 메시지를 읽고 메시지 명령어에 해당하는 동작을 취하도록 함
    // i : client index 
    for(int i = 0; i < active_client_count; i++){
        // 비활성 클라이언트는 건너뛰기
        if(clients[i].pid == 0){
            continue;
        }

        while(1) {
            memset(buf, 0, BUFSIZ); // 버퍼를 0 으로 초기화
            // non-blocking 으로 읽기 시도
            n = read(pipe_child_to_parent[i][0], buf, BUFSIZ);
            if (n <= 0) {
                break; // 더 이상 읽을 게 없으면 break
            }
            
            // 메시지를 보낸 첫 자식을 찾았을 때 처리
            buf[n] = '\0'; // 문자열 끝 처리
            // 7단계 : LOG Redirection
            char logMsg[BUFSIZ * 2 + 32];
            char errMsg[BUFSIZ * 2];
            snprintf(errMsg, sizeof(errMsg), "[INFO] : SIGUSR1 핸들러: 클라이언트 index %d 로부터 메시지 수신을 담당 서버 자식프로세스로부터 받음 : %s", i, buf); // 로그 TYPE 문자열 결합
            get_timestamp(logMsg, sizeof(logMsg), errMsg);
            printf("\n%s", logMsg); // 로그에 현재 시간 + 관련 로그 출력
            fflush(stdout);

            char ch[10], str[BUFSIZ + 12 + 50];
            // 클라이언트로부터 받은 문자열 분리
            // 클라이언트로부터 받는 문자열 예시 1 : /NICK NICKNAME
            // 예시 2 : /MSG NICKNAME:MSG
            // chat-dev2 : 버그 수정 - 메시지에 공백이 있을 때 공백을 메시지에 포함하지 못하는 경우 수정
            // => sscanf 는 공백 포함 문자열을 담기 어렵기 때문에 strchr 과 strcpy 구조로 변경
            char* space = strchr(buf, ' ');
            if (space != NULL) {
                sscanf(buf, "/%s", ch);
                strcpy(str, space + 1);  // 공백 이후 문자열 복사
            }

            // 닉네임 중복 검사 처리
            if(strcmp(ch, "NICK") == 0){
                int is_dup = 0;
                for(int j = 0; j < active_client_count; j++){
                    if(clients[j].pid != 0 && j != i && strcmp(clients[j].nickName, str) == 0){
                        is_dup = 1; // 중복 처리
                        break;
                    } 
                }
                char response[BUFSIZ + 12 + 50];
                if(is_dup){ // 중복
                    snprintf(response, sizeof(response), "%s", "DUP");
                } else {
                    // 중복이 아닐 때 nickName 부여
                    strncpy(clients[i].nickName, str, sizeof(clients[i].nickName) - 1);
                    snprintf(response, sizeof(response), "%s", "OK");
                }
                
                // 중복 처리 결과를 i 번 자식 파이프에 write
                write(pipe_parent_to_child[i][1], response, strlen(response));
                kill(clients[i].pid, SIGUSR2); // 해당 자식 프로세스에 SIGUSR2 시그널 알림
            } else if(strcmp(ch, "MSG") == 0){
                // 같은 채팅방에만 전송하기 위해서 사용할 임시 변수 sender_room
                int sender_room = clients[i].room_idx;
                
                // 브로드캐스트할 전체 채팅 메시지
                char sendnickName[51];
                char msg[BUFSIZ];
                char* colon = strchr(str, ':');
                if (colon != NULL) {
                    *colon = '\0'; // ':'를 문자열 종료로 바꿈
                    strcpy(sendnickName, str);
                    strcpy(msg, colon + 1);
                }
                char broadcast_msg[BUFSIZ * 3];
                
                // chat-dev2 : 채팅을 보낼 때 무슨 채팅방에서 보냈는지 를 닉네임 앞에 추가함
                char WhereIsRoomAndNickname[BUFSIZ * 2];
                snprintf(WhereIsRoomAndNickname, sizeof(WhereIsRoomAndNickname), "%s 채널(%d) ", rooms[sender_room].roomName, sender_room);
                strcat(WhereIsRoomAndNickname, sendnickName);

                snprintf(broadcast_msg, sizeof(broadcast_msg), "/MSG %s:%s", WhereIsRoomAndNickname, msg);

                // pid 가 0 이 아니고(실제 접속 중인 클라이언트 서버한테만) 같은 채팅 공간에 브로드캐스트 메시지를 j 번 파이프에 write
                // 하고, 해당 자식 프로세스에 SIGUSR2 시그널 알림
                for(int j = 0; j < active_client_count; j++){
                    if (clients[j].pid > 0 && clients[j].room_idx == sender_room) {
                        write(pipe_parent_to_child[j][1], broadcast_msg, strlen(broadcast_msg));
                        kill(clients[j].pid, SIGUSR2);
                    }
                }
                // chat-dev2 : 채팅방 개설 명령 추가
                // 서버에서 체크 사항 : 채팅방 최대 수용량 체크, 채팅방 이름 중복 여부 확인 후  
                // 허용 가능할 때 roomData 의 is_active 를 활성화시키고, 요청한 클라이언트의 clientData 의 room_idx 를 해당 room 으로 변경한다. 
            } else if(strcmp(ch, "ADD") == 0){
                // chat-dev2 : 클라이언트의 부모 프로세스로 부터 받은 문자열을 받고 
                // 명령어에 따라 문자열 파싱 + 파이프에 write + 현재(서버)의 부모 프로세스로 시그널 알림 동작이 발생함
                // chat-dev2 : /add 채팅방 추가
                // 서버에서 체크 사항 : 채팅방 최대 수용량 체크, 채팅방 이름 중복 여부 확인 후  
                // 허용 가능할 때 roomData 의 is_active 를 활성화시키고, 요청한 클라이언트의 clientData 의 room_idx 를 해당 room 으로 변경한다. 
                
                char tempMsg[BUFSIZ];
                int is_valid = 0; // 채팅방 개설 가능 여부 변수

                // 채팅방 최대 수용량 및 채팅방 이름 중복 여부 확인
                int k;
                for (k = 0; k < MAX_ROOMS; k++){
                    if(strcmp(rooms[k].roomName, str) == 0){
                        // 중복 처리
                        snprintf(tempMsg, sizeof(tempMsg), "/ADD %s", "DUP");  
                        break;
                    }
                    if(rooms[k].is_active == 0){
                        // is_active = 0 이므로 채팅방 활성화 가능
                        is_valid = 1;
                        snprintf(tempMsg, sizeof(tempMsg), "/ADD %d%s", k, str);
                        break;
                    }
                }

                // 활성화된 채팅방 없음 (모두 is_active = 1)
                if(k == MAX_ROOMS && is_valid == 0){
                    snprintf(tempMsg, sizeof(tempMsg), "/ADD %s", "INVALID");
                }

                ///
                char ch2[10], str2[BUFSIZ + 12 + 50];
                char* space = strchr(tempMsg, ' ');
                if (space != NULL) {
                    sscanf(tempMsg, "/%s", ch2);
                    strcpy(str2, space + 1);  // 공백 이후 문자열 복사
                }

                char sendMsg[500];
                if(strcmp(str2, "DUP") == 0){
                    snprintf(sendMsg, sizeof(sendMsg), "/ADD %s", "중복된 채팅방 이름입니다.\n");
                } else if(strcmp(str2, "INVALID") == 0){
                    snprintf(sendMsg, sizeof(sendMsg), "/ADD %s", "채팅방 최대 수용량을 초과하였습니다.\n");
                } else {
                    // 허용 가능
                    int inputRoomNum;
                    char inputRoomName[100];
                    
                    // 숫자와 문자열 분리
                    sscanf(str2, "%d%s", &inputRoomNum, inputRoomName);
                    rooms[inputRoomNum].is_active = 1; // 채팅방 활성화함
                    strcpy(rooms[inputRoomNum].roomName, inputRoomName); // 활성화한 채팅방 이름 변경
                    clients[i].room_idx = inputRoomNum; // 클라이언트의 채팅방 위치 변경

                    snprintf(sendMsg, sizeof(sendMsg), "/ADD %d 번째 %s 채팅방을 만들고 입장했습니다.", inputRoomNum, inputRoomName);
                }
                
                // 서버 부모 프로세스에서 처리(컨트롤) 후 결과를 서버 자식 프로세스(해당 클라이언트 담당) 에게 전달할 파이프에 작성
                write(pipe_parent_to_child[i][1], sendMsg, strlen(sendMsg));
                // 후 서버 자식 프로세스(해당 클라이언트 담당 프로세스)에게 시그널 알림
                kill(clients[i].pid, SIGUSR2); 
            } // chat-dev3 : /LEAVE 명령어. 현재 클라이언트가 로비 채널이 아닌 채팅방에 있을 때만, 로비 채널로 이동 시켜 준다.
            else if(strcmp(ch, "LEAVE") == 0){
                char sendMsg[500];

                char ch[10], str[BUFSIZ + 12 + 50];
                char* space = strchr(buf, ' ');
                if (space != NULL) {
                    sscanf(buf, "/%s", ch);
                    strcpy(str, space + 1);  // 공백 이후 문자열 복사
                }

                // 이미 로비에서 Leave 명령어 수행 시 동작하지 않음
                if(strcmp(str, "lobby") == 0){
                    if(clients[i].room_idx == 0){
                        snprintf(sendMsg, sizeof(sendMsg), "%s", "/LEAVE 이미 로비(Lobby) 채널에 있는 유저입니다.");    
                    } else {
                        // 로비가 아닌 다른 채팅방에 있는 클라이언트일 경우 로비 채널로 이동
                        clients[i].room_idx = 0;
                        snprintf(sendMsg, sizeof(sendMsg), "%s", "/LEAVE 로비(Lobby) 채널로 이동합니다.");
                    }
                } else {
                    snprintf(sendMsg, sizeof(sendMsg), "%s", "/LEAVE 잘못된 명령 문구를 입력했습니다.");    
                }
                
                write(pipe_parent_to_child[i][1], sendMsg, strlen(sendMsg));
                kill(clients[i].pid, SIGUSR2);
            }
            
            // 추후 /join, /leave 등 다른 명령어 처리 로직 추가 예정
        }
    }
}

// chat-dev1 : sigusr2 핸들러 (자식에서 클라이언트 서버에 메시지 or 데이터 전달)
void child_sigusr2_handler(int signo){
    // 7단계 : LOG Redirection
    char logMsg[BUFSIZ * 2 + 32];
    char errMsg[BUFSIZ * 2];
    snprintf(errMsg, sizeof(errMsg), "[INFO] : [자식 pid %d] SIGUSR2 핸들러 발생", getpid()); // 로그 TYPE 문자열 결합
    get_timestamp(logMsg, sizeof(logMsg), errMsg);
    printf("\n%s", logMsg); // 로그에 현재 시간 + 관련 로그 출력
    fflush(stdout);

    char buf[BUFSIZ + 10 + 50];
    int n;

    // pipe 에서 데이터를 읽고 클라이언트 서버에 write
    while(1){
        memset(buf, 0, BUFSIZ); // 버퍼 초기화
        n = read(pipe_parent_to_child[child_index][0], buf, sizeof(buf)-1);

        if(n <= 0){ // 읽을 데이터가 없거나(n=0 또는 n=-1), 에러 발생 시 루프 종료
            break;
        }
        if (n > 0) {
            buf[n] = '\0'; // 문자열 끝 처리
            write(clients[child_index].client_sock_fd, buf, n); // 클라이언트에게 전송
        }
    }
}

// 5단계 : 좀비 프로세스(부모 프로세스가 종료되어도 자식의 "종료" 상태(ex. pid) 가 커널에 남아 있는 상태 - 자원을 사용하진 않음) 회수용
// chat-dev1 : sigchld 좀비 프로세스 처리(close for clear) 함수 수정(struct 사용에 따라 수정)
void handle_sigchld(int signo) {
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            // 파이프 및 클라이언트 소켓 닫기
            if (clients[i].pid == pid) {
                // 7단계 : LOG Redirection
                char logMsg[BUFSIZ * 2 + 32];
                char errMsg[BUFSIZ * 2];
                snprintf(errMsg, sizeof(errMsg), "[INFO] : 클라이언트 %d (pid: %d, nick: %s) 접속 종료. 자원 회수 완료.\n", i, pid, clients[i].nickName); // 로그 TYPE 문자열 결합
                get_timestamp(logMsg, sizeof(logMsg), errMsg);
                printf("\n%s", logMsg); // 로그에 현재 시간 + 관련 로그 출력
                fflush(stdout);

                close(clients[i].client_sock_fd);
                close(pipe_child_to_parent[i][0]);
                close(pipe_parent_to_child[i][1]);
                // 해당 pid 가 있는 clients 인덱스 에서 pid 0 처리 포함 memset
                memset(&clients[i], 0, sizeof(ClientData)); // 슬롯 초기화
                break;
            }
        }
    }
}

// 6단계 : Graceful shutdown 핸들러 추가
// 고아 프로세스(부모 프로세스가 먼저 종료된 후 자식 프로세스가 여전히 "실행 중" 인 상태 - 실제 자원을 사용)
// chat-dev1 : clients struct 데이터 구조에 따른 구조 수정
void graceful_shutdown_handler(int signo) {
    // 7단계 : LOG Redirection
    char logMsg[BUFSIZ * 2 + 32];
    char errMsg[BUFSIZ * 2];
    snprintf(errMsg, sizeof(errMsg), "[INFO] : [부모 pid %d] 서버 종료 시그널 수신 : 모든 자식 종료 중 ...", getpid()); // 로그 TYPE 문자열 결합
    get_timestamp(logMsg, sizeof(logMsg), errMsg);
    printf("\n%s", logMsg); // 로그에 현재 시간 + 관련 로그 출력
    fflush(stdout);

    for (int i = 0; i < active_client_count; i++) {
        if (clients[i].pid > 0) {
            // 활성화된 clients struct 에서 pid 가 활성화된 자식만 종료 요청 
            kill(clients[i].pid, SIGTERM);
            waitpid(clients[i].pid, NULL, 0); // 자식 PID 초기화 및 종료될 때까지 기다림
        }
    }
    close(listen_fd);
    close(file_fd);

    // 7단계 : LOG Redirection
    char logMsg2[BUFSIZ * 2 + 32];
    char errMsg2[BUFSIZ * 2];
    snprintf(errMsg2, sizeof(errMsg2), "[INFO] : [부모 pid %d] 서버 종료 완료. 자원 회수 완료.", getpid()); // 로그 TYPE 문자열 결합
    get_timestamp(logMsg2, sizeof(logMsg2), errMsg2);
    printf("\n%s", logMsg2); // 로그에 현재 시간 + 관련 로그 출력
    fflush(stdout);

    exit(0);
}

// 6 단계 : 자식 프로세스 쪽 sigterm handler
void child_sigterm_handler(int signo) {
    // 7단계 : LOG Redirection
    char logMsg[BUFSIZ * 2 + 32];
    char errMsg[BUFSIZ * 2];
    snprintf(errMsg, sizeof(errMsg), "[INFO] : [자식 pid %d] 종료 시그널 수신. 종료 중...", getpid()); // 로그 TYPE 문자열 결합
    get_timestamp(logMsg, sizeof(logMsg), errMsg);
    printf("\n%s", logMsg); // 로그에 현재 시간 + 관련 로그 출력
    fflush(stdout);

    close(clients[child_index].client_sock_fd); // 클라이언트와 연결된 소켓 닫기
    close(pipe_child_to_parent[child_index][1]); // 부모에게 쓰는 파이프 닫기
    close(pipe_parent_to_child[child_index][0]); // 부모로부터 읽는 파이프 닫기

    exit(0); 
}

// sigaction 커스텀 함수
/*
    sigaction : signal 로 시그널이 발생할 때 다시 동일한 시그널이 발생할 때
        이전 시그널을 처리하는 동안 시그널이 블록되어 처리되지 않고 버려지는 경우를 해결하는 
        더 향상된 시그널 처리 함수
*/
void register_sigaction(int signo, void (*handler)(int)) {
    // 시그널 처리를 위한 시그널 액션
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    // 터미널 제어 관련 시그널 관리
    sa.sa_handler = handler; // 시그널 발생 시 실행할 핸들러 지정
    sigemptyset(&sa.sa_mask); // 모든 시그널 허용
    sa.sa_flags = SA_RESTART; // 시그널 처리에 의해 방해받은 시스템 호출을 시그널 처리가 끝나면 재시작

    // 시그널 처리 동작 처리
    if (sigaction(signo, &sa, NULL) == -1) {
        // 7단계 : LOG Redirection
        char logMsg[BUFSIZ * 2 + 32];
        char errMsg[BUFSIZ * 2];
        snprintf(errMsg, sizeof(errMsg), "[ERROR] : 시그널 처리 동작이 실패하였습니다."); // 로그 TYPE 문자열 결합
        get_timestamp(logMsg, sizeof(logMsg), errMsg);
        printf("\n%s", logMsg); // 로그에 현재 시간 + 관련 로그 출력
        fflush(stdout);

        exit(1);
    }
}

// 7 단계 : 서버 데몬화 처리 
// -> 서버 실행 시 백그라운드로 전환하고, printf 들을 별도 데일리 로그 파일에서 출력하도록 리디렉션 처리
void daemonize_with_log() {
    pid_t pid;

    umask(0); // 파일 생성을 위한 마스크를 0 으로 설정

    // fork() 로 자식 프로세스를 생성 하고 부모 프로세스는 종료
    pid = fork();
    if (pid < 0) exit(1);
    if (pid > 0) exit(0); // 부모 종료

    // 자식 프로세스를 세션 리더로 만들어 터미널 조작을 못하게 함
    if (setsid() < 0) exit(1);

    chdir("./"); // 현재 디렉토리로 이동

    // 표준 입력 닫기
    close(STDIN_FILENO);

    // 로그 디렉토리 생성 (이미 있는 경우는 무시하도록 함)
    if(mkdir("./logs", 0755) == -1){
        if(errno != EEXIST){ // 디렉토리가 이미 있으면 무시 또는 생성 실패 시 처리
            perror("mkdir");
            exit(EXIT_FAILURE);
        }
    }

    // 날짜별 로그 파일명 생성
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    char logfile[256];
    snprintf(logfile, sizeof(logfile), "./logs/chattingServer_%04d%02d%02d.log",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);

    // 로그 파일 열기
    file_fd = open(logfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (file_fd < 0) exit(1); // 로그 파일 열기 실패 시 종료

    // stdout, stderr 을 로그 파일로 리디렉션
    dup2(file_fd, STDOUT_FILENO); // printf()
    dup2(file_fd, STDERR_FILENO); // perror(), fprintf(stderr, ...)
}

int main(int argc, char** argv) {
    // 데이터 구조 초기화
    memset(clients, 0, sizeof(clients));
    memset(rooms, 0, sizeof(rooms));
    strcpy(rooms[0].roomName, "Lobby");
    rooms[0].is_active = 1;

    // 7 단계 : 서버 데몬화 처리
    daemonize_with_log();

    // 4단계: 부모에서 시그널 핸들러 SIGUSR1 등록
    register_sigaction(SIGUSR1, sigusr1_handler); 
    // 5단계 : 좀비 프로세스(자식이 종료된 후 PID 만 남아서 자원 누수가 발생하는 프로세스) 방지
    // -> 자식이 종료될 경우 자원을 회수하여 좀비 프로세스가 남지 않도록 함
    register_sigaction(SIGCHLD, handle_sigchld); 
    // 6단계 : 부모 프로세스 Graceful shutdown 핸들러 추가
    // 고아 프로세스(부모 프로세스가 먼저 종료된 후 자식 프로세스가 여전히 "실행 중" 인 상태 - 실제 자원을 사용)
    register_sigaction(SIGINT, graceful_shutdown_handler);
    register_sigaction(SIGTERM, graceful_shutdown_handler);

    struct sockaddr_in serv_addr;

    // 1 단계 : TCP 소켓 생성(socket())
    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        // 7단계 : get_timestamp 에서 에러 발생 시간과 함께 로그 데이터를 출력하기 위해서 perror 대신에 문자열을 반환해주는
        // strerror(errno) 를 사용한다.

        // 7단계 : LOG Redirection
        char logMsg[BUFSIZ * 2 + 32];
        char errMsg[BUFSIZ * 2];
        snprintf(errMsg, sizeof(errMsg), "[ERROR] : %s", strerror(errno)); // 로그 TYPE 문자열 결합
        get_timestamp(logMsg, sizeof(logMsg), errMsg);
        printf("\n%s", logMsg); // 로그에 현재 시간 + 관련 로그 출력
        fflush(stdout);

        close(file_fd); // 로그 파일 디스크립터 닫음
        return -1;
    }

    // 1 단계 : 서버 주소 구조체 설정(memset 후 server 주소 구조체 설정)
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(PORT);

    // 1 단계 : 소켓에 서버 주소 바인딩(bind())
    if (bind(listen_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1) {
        // 7단계 : LOG Redirection
        char logMsg[BUFSIZ * 2 + 32];
        char errMsg[BUFSIZ * 2];
        snprintf(errMsg, sizeof(errMsg), "[ERROR] : %s", strerror(errno)); // 로그 TYPE 문자열 결합
        get_timestamp(logMsg, sizeof(logMsg), errMsg);
        printf("\n%s", logMsg); // 로그에 현재 시간 + 관련 로그 출력
        fflush(stdout);

        close(listen_fd);
        close(file_fd); // 로그 파일 디스크립터 닫음
        return -1;
    }

    // 1 단계 : 클라이언트 연결 대기(listen())
    if (listen(listen_fd, PENDING_CONN) < 0) {
        // 7단계 : LOG Redirection
        char logMsg[BUFSIZ * 2 + 32];
        char errMsg[BUFSIZ * 2];
        snprintf(errMsg, sizeof(errMsg), "[ERROR] : %s", strerror(errno)); // 로그 TYPE 문자열 결합
        get_timestamp(logMsg, sizeof(logMsg), errMsg);
        printf("\n%s", logMsg); // 로그에 현재 시간 + 관련 로그 출력
        fflush(stdout);
        
        close(listen_fd);
        close(file_fd); // 로그 파일 디스크립터 닫음
        return -1;
    }

    // 7단계 : LOG Redirection
    char logMsg[BUFSIZ * 2 + 32];
    char errMsg[BUFSIZ * 2];
    snprintf(errMsg, sizeof(errMsg), "[INFO] : 서버가 %d 번 포트에서 대기하고 있습니다......\n", PORT); // 로그 TYPE 문자열 결합
    get_timestamp(logMsg, sizeof(logMsg), errMsg);
    printf("\n%s", logMsg); // 로그에 현재 시간 + 관련 로그 출력
    fflush(stdout);

    while (1) {
        struct sockaddr_in cli_addr;
        // 2 단계 : 클라이언트 연결 수락(accept())
        socklen_t cli_len = sizeof(cli_addr);
        conn_fd = accept(listen_fd, (struct sockaddr*)&cli_addr, &cli_len);
        if (conn_fd < 0) {
            // 7단계 : LOG Redirection
            char logMsg[BUFSIZ * 2 + 32];
            char errMsg[BUFSIZ * 2];
            snprintf(errMsg, sizeof(errMsg), "[ERROR] : %s", "accept() - 클라이언트 연결을 수락하지 못했습니다."); // 로그 TYPE 문자열 결합
            get_timestamp(logMsg, sizeof(logMsg), errMsg);
            printf("\n%s", logMsg); // 로그에 현재 시간 + 관련 에러 로그 출력
            fflush(stdout);
            
            continue;
        }

        // 6단계 : 새 클라이언트를 위한 빈 슬롯(인덱스) 찾기
        int new_client_idx = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].pid == 0) { // child_pid 가 0 이면 비어있는 슬롯
                new_client_idx = i; // 비어있는 슬롯에 새 클라이언트 idx 할당하기 위함
                break;
            }
        }

        // 6 단계 : 빈 슬롯이 없을 때 (서버 꽉 찬 상태)
        if(new_client_idx == -1){
            // 7단계 : LOG Redirection
            char logMsg[BUFSIZ * 2 + 32];
            char errMsg[BUFSIZ * 2];
            snprintf(errMsg, sizeof(errMsg), "[ERROR] : %s", "서버 수용량 초과로 접속할 수 없습니다."); // 로그 TYPE 문자열 결합
            get_timestamp(logMsg, sizeof(logMsg), errMsg);
            printf("\n%s", logMsg); // 로그에 현재 시간 + 관련 에러 로그 출력
            fflush(stdout);

            write(conn_fd, "서버가 꽉 찼습니다.\n", strlen("서버가 꽉 찼습니다.\n"));
            close(conn_fd);
            continue; // 다음 accept() 대기로
        }

        // 7 단계 : LOG Redirection
        char logMsg[BUFSIZ * 2 + 32];
        char errMsg[BUFSIZ * 2];
        snprintf(errMsg, sizeof(errMsg), "[INFO] : 클라이언트 연결됨: %s", inet_ntoa(cli_addr.sin_addr)); // 로그 TYPE 문자열 결합
        get_timestamp(logMsg, sizeof(logMsg), errMsg);
        printf("\n%s", logMsg); // 로그에 현재 시간 + 관련 로그 출력

        // 3 -> 4단계: pipe 생성 (자식마다)
        // 4 -> 6단계 : 찾은 인덱스(new_client_idx)를 사용하여 파이프 생성
        if (pipe(pipe_child_to_parent[new_client_idx]) == -1 ||
            pipe(pipe_parent_to_child[new_client_idx]) == -1) {
            // 7단계 : LOG Redirection
            char logMsg[BUFSIZ * 2 + 32];
            char errMsg[BUFSIZ * 2];
            snprintf(errMsg, sizeof(errMsg), "[ERROR] : %s", "pipe - 새 클라이언트와 연결하기 위한 파이프 생성에 실패하였습니다."); // 로그 TYPE 문자열 결합
            get_timestamp(logMsg, sizeof(logMsg), errMsg);
            printf("\n%s", logMsg); // 로그에 현재 시간 + 관련 에러 로그 출력
            fflush(stdout);

            close(conn_fd);
            continue;
        }

        // 3 단계 : 자식 프로세스 생성(fork())
        pid_t pid = fork();
        if (pid < 0) {
            // 7단계 : LOG Redirection
            char logMsg[BUFSIZ * 2 + 32];
            char errMsg[BUFSIZ * 2];
            snprintf(errMsg, sizeof(errMsg), "[ERROR] : %s", "fork() - 새 클라이언트와 연결하기 위한 자식 프로세스 생성에 실패하였습니다."); // 로그 TYPE 문자열 결합
            get_timestamp(logMsg, sizeof(logMsg), errMsg);
            printf("\n%s", logMsg); // 로그에 현재 시간 + 관련 에러 로그 출력
            fflush(stdout);

            close(conn_fd);
            continue;
        } else if (pid == 0) { // 자식 프로세스일 때의 처리
            child_index = new_client_idx;
            clients[child_index].client_sock_fd = conn_fd; // 자식만 자신의 fd를 구조체에 기록
            close(listen_fd); // 서버가 클라이언트 연결을 기다리기 위한 소켓(서버 대기용) 닫음

            // 6 단계 : 자식이 sigterm 을 받을 때, 정리하기 위한 핸들러 추가
            register_sigaction(SIGTERM, child_sigterm_handler);
            register_sigaction(SIGINT, child_sigterm_handler);
            // 4 단계: 자식에서 시그널 핸들러 SIGUSR2 등록
            register_sigaction(SIGUSR2, child_sigusr2_handler);

            // 6 단계 : 새로 찾은 인덱스를 자신의 인덱스(자식)으로 사용
            child_index = new_client_idx; // 자식 전용 인덱스 설정

            // 파이프 정리
            close(pipe_child_to_parent[child_index][0]); // 부모는 pipe_child_to_parent 파이프에서 write 만 유지
            close(pipe_parent_to_child[child_index][1]); // 부모는 pipe_parent_to_child 파이프에서 read 만 유지

            // 자식이 읽는 파이프를 non-blocking 으로 설정 (부모 write 가 막히지 않도록 하기 위함)
            int flags = fcntl(pipe_parent_to_child[child_index][0], F_GETFL, 0);
            fcntl(pipe_parent_to_child[child_index][0], F_SETFL, flags | O_NONBLOCK);
            
            // 자식은 클라이언트의 모든 메시지를 부모에게 전달만 함
            char buf[BUFSIZ + 50 + 10];
            int n;
            while (1) {
                memset(buf, 0, BUFSIZ); // 버퍼를 0 으로 초기화
                // chat-dev2 : 클라이언트로부터 받은 문자열이 / 으로 들어오게 됨
                n = read(conn_fd, buf, sizeof(buf)-1);

                // 6 단계 : read() 가 <= 0 일 때 graceful 연결 종료 처리를 위한 부분 처리
                if (n <= 0) {
                    // 7단계 : LOG Redirection
                    char logMsg[BUFSIZ * 2 + 32];
                    char errMsg[BUFSIZ * 2];
                    snprintf(errMsg, sizeof(errMsg), "[WARNING] : [자식 index %d, pid %d] 클라이언트 연결 종료가 감지되어 해당 클라이언트 연결을 종료합니다.", child_index, getpid()); // 로그 TYPE 문자열 결합
                    get_timestamp(logMsg, sizeof(logMsg), errMsg);
                    printf("\n%s", logMsg); // 로그에 현재 시간 + 관련 로그 출력
                    fflush(stdout);

                    close(clients[child_index].client_sock_fd);
                    break;
                }

                // 종료 조건 : 'q' 로 메시지가 입력될 때 자식을 graceful 종료 처리
                if (strcmp(buf, "q") == 0) {
                    // 7단계 : LOG Redirection
                    char logMsg[BUFSIZ * 2 + 32];
                    char errMsg[BUFSIZ * 2];
                    snprintf(errMsg, sizeof(errMsg), "[INFO] : [pid %d] 클라이언트로부터의 종료 요청 수신으로 해당 클라이언트 연결을 종료합니다.", getpid()); // 로그 TYPE 문자열 결합
                    get_timestamp(logMsg, sizeof(logMsg), errMsg);
                    printf("\n%s", logMsg); // 로그에 현재 시간 + 관련 로그 출력
                    fflush(stdout);

                    close(clients[child_index].client_sock_fd); // 자식에서 종료 시 자신의 conn_fd 를 닫아야 함
                    break;
                }

                // 자식 → 부모 전송
                buf[n] = '\0'; // 문자열 끝 처리

                // 자식 프로세스에서 서버 부모 프로세스에 데이터를 파이프 작성으로 통해서 전달하도록 함
                write(pipe_child_to_parent[child_index][1], buf, n); // 3->4단계: 자식 → 부모로 write
                // 7단계 : LOG Redirection
                char logMsg[BUFSIZ * 2 + 32];
                char errMsg[BUFSIZ * 2];
                snprintf(errMsg, sizeof(errMsg), "[INFO] : [자식 index %d, pid : %d] 서버의 부모 프로세스에게 메시지(데이터) 작성 SIGNAL 알림: %s", child_index, getpid(), buf); // 로그 TYPE 문자열 결합
                get_timestamp(logMsg, sizeof(logMsg), errMsg);
                printf("\n%s", logMsg); // 로그에 현재 시간 + 관련 로그 출력
                fflush(stdout);

                kill(getppid(), SIGUSR1);
            }
            exit(0);  // 자식 프로세스 종료
        } else { // 부모 프로세스
            // 부모는 자식에게 conn_fd 를 넘기고 자신의 copy 된 conn_fd 는 닫고, 자식에서 종료 시 자신의 conn_fd 를 닫아야 함
            close(conn_fd); // 부모에서는 conn_fd(연결 소켓) 를 사용하지 않음

            // 부모가 클라이언트 정보 관리
            clients[new_client_idx].pid = pid;
            clients[new_client_idx].client_sock_fd = conn_fd; // conn_fd를 저장하지만 부모가 직접 사용하진 않음
            strcpy(clients[new_client_idx].nickName, "GUEST"); // 임시 닉네임
            clients[new_client_idx].room_idx = 0; // 기본적으로 로비에 참가

            close(pipe_child_to_parent[new_client_idx][1]); // 부모는 child_to_parent 파이프에서 read 만 유지
            close(pipe_parent_to_child[new_client_idx][0]); // 부모는 parent_to_child 파이프에서 write 만 유지

            // 부모가 읽는 파이프를 non-blocking 모드로 설정해 핸들러가 멈추지 않도록 함
            int flags = fcntl(pipe_child_to_parent[new_client_idx][0], F_GETFL, 0);
            fcntl(pipe_child_to_parent[new_client_idx][0], F_SETFL, flags | O_NONBLOCK);

            // 6단계 : client_index 를 루프의 최대 경계로 사용하기 위해 업데이트
            if (new_client_idx >= active_client_count) {
                active_client_count = new_client_idx + 1;
            }
        }
    }

    close(file_fd); // 로그 파일 디스크립터 닫음
    close(listen_fd);
    return 0;
}