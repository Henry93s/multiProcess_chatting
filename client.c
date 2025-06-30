#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>

// chat-dev5 : ANSI 이스케이프 코드를 사용하여 글자에 색상을 넣기 위한 색 DEFINE
#define COLOR_RED     "\x1b[31m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_BLUE    "\x1b[34m"
#define COLOR_MAGENTA "\x1b[35m"
#define COLOR_CYAN    "\x1b[36m"
#define COLOR_RESET   "\x1b[0m"

#define PORT    5101

int sockfd; // 소켓 파일 디스크립터
char nickname[51]; // 닉네임

// 0625 구조 수정 : 전역 변수로 자식 -> 부모 데이터 파이프 선언
int pipe_child_to_parent[2];

// chat-dev5 : ANSI 이스케이프 코드를 사용하여 필요 시 화면 clear 기능을 사용하도록 함
// 위의 선언없이 extern inline void clrscr(void)로 선언
inline void clrscr(void);		// C99, C11에 대응하기 위해서 사용
void clrscr(void)				
{
    write(1, "\033[1;1H\033[2J", 10);		// ANSI escape 코드로 화면 지우기
}

// 6 단계 : 클라이언트에서 fork된 자식(수신용) 프로세스가 종료되었을 때 부모가 기다리지 않아서 발생하는 좀비 프로세스 방지 
void handle_sigchld(int signo) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
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
        perror("register_sigaction()");
        exit(1);
    }
}

// 프로젝트 제한사항에 따른 client 구조 수정
// => sigusr1_handler 를 client 부모 프로세스에 등록하고 client 자식 프로세스에서 SIGNAL 알림을 보내면
// client 부모 프로세스에서 server 의 자식 프로세스(해당 클라이언트 담당 프로세스) 에 write 
// => client 부모 프로세스에서 server 의 자식 프로세스(해당 클라이언트 담당 프로세스) 에서 read 
void sigusr1_handler(int signo){
    // client 자식 프로세스에서 메시지 입력 감지 시그널 알림으로 client 부모 프로세스에서 이벤트 동작 시작
    char buf[BUFSIZ + 10 + 50];
    int n;

    memset(buf, 0, BUFSIZ);

    // command 동작
    char ch[10];
    char str[BUFSIZ];

    // non-blocking read
    n = read(pipe_child_to_parent[0], buf, BUFSIZ);
    if(n <= 0){
        return; // 읽은 데이터 없음
    }
    buf[n] = '\0';

    // chat-dev2 : 명령어 동작일 경우 있는 buf 파이프에 있는 그대로 문자열을 보냄
    if(buf[0] == '/'){
        // 파이프에 있는 문자열을 서버로 보냄
        write(sockfd, buf, strlen(buf));
    } else {
        // client 자식으로부터 받은 버퍼 메시지 문자열 분리
        // chat-dev2 : 버그 수정 - 메시지에 공백이 있을 때 공백을 메시지에 포함하지 못하는 경우 수정
        // => sscanf 는 공백 포함 문자열을 담기 어렵기 때문에 strchr 과 strcpy 구조로 변경
        // chat-dev5 : /WHISPER 사용자이름 - 서버에 접속한 사용자에게만 귓속말 전달
        char* space = strchr(buf, ' ');
        if (space != NULL) {
            sscanf(buf, "/%s", ch);
            strcpy(str, space + 1);  // 공백 이후 문자열 복사
        }
        
        if(ch == NULL){
            return;
        }
        // chat-dev5 : /WHISPER 사용자이름 - 서버에 접속한 사용자에게만 귓속말 전달
        if(strcmp(ch, "MSG") == 0 || strcmp(ch, "WHISPER") == 0){
            char nickName[51];
            char msg[BUFSIZ];

            // 채팅 메시지를 닉네임과 채팅 msg 로 분리
            char* colon = strchr(str, ':');
            if (colon != NULL) {
                *colon = '\0'; // ':'를 문자열 종료로 바꿈
                strcpy(nickName, str);
                strcpy(msg, colon + 1);

                // 0625 구조 수정 : 시그널을 받고 파이프에 있는 데이터를 서버에 전달
                // 서버로 보낼 메시지 프로토콜 생성
                char sendMsg[BUFSIZ + 12 + 50];
                // 서버에 보낼 문자열 결합
                if(strcmp(ch, "MSG") == 0){
                    snprintf(sendMsg, sizeof(sendMsg), "/MSG %s:%s", nickname, msg);
                } else if(strcmp(ch, "WHISPER") == 0){
                    snprintf(sendMsg, sizeof(sendMsg), "/WHISPER %s:%s", nickname, msg);
                }
                
                // 파이프에 있는 문자열을 서버로 보냄
                write(sockfd, sendMsg, strlen(sendMsg));
            }
        } 
    }
}

// 서버로부터 메시지를 받아 파싱하고 출력하는 함수
// 부모 read : 메시지 파싱 후 동작
void process_server_message(char *buf) {
    // command 동작
    char ch[10];
    char str[BUFSIZ];

    // 서버로부터 받은 버퍼 문자열 분리
    // chat-dev2 : 버그 수정 - 메시지에 공백이 있을 때 공백을 메시지에 포함하지 못하는 경우 수정
    // => sscanf 는 공백 포함 문자열을 담기 어렵기 때문에 strchr 과 strcpy 구조로 변경
    // chat-dev5 : /WHISPER 사용자이름 - 서버에 접속한 사용자에게만 귓속말 전달
    char* space = strchr(buf, ' ');
    if (space != NULL) {
        sscanf(buf, "/%s", ch);
        strcpy(str, space + 1);  // 공백 이후 문자열 복사
    }

    if (ch == NULL) {
        return;
    }

    if (strcmp(ch, "MSG") == 0 || strcmp(ch, "WHISPER") == 0) {
        char nickName[51];
        char msg[BUFSIZ];

        // 채팅 메시지는 닉네임과 채팅(msg) 를 분리
        // 클라이언트로부터 받은 채팅 메시지 분리
        char* colon = strchr(str, ':');
        if (colon != NULL) {
            *colon = '\0'; // ':'를 문자열 종료로 바꿈
            strcpy(nickName, str);
            strcpy(msg, colon + 1);

            // chat-dev5 : 귓속말일 경우 YELLOW 색 출력하고 색 RESET
            if(strcmp(ch, "WHISPER") == 0){
                printf(COLOR_YELLOW "\n[%s] >>> %s\n" COLOR_RESET, nickName, msg);
            } else {
                // 메시지 출력
                printf("\n[%s] >>> %s\n", nickName, msg);
            }
            fflush(stdout);  // 입력줄 깨지지 않도록
        } 
    } // chat-dev2 : 서버로 부터 채팅방 개설 요청에 대한 결과를 받고, 이를 클라이언트에 처리 결과를 알림
    // chat-dev3 : /LEAVE 명령어. 서버로부터 처리와 처리 결과를 반환 받고 메시지를 출력
    // chat-dev4 : /USER all - 서버에 접속한 전체 유저 정보를 출력, /USER 채팅방이름 - 서버의 특정 채팅 채널방에 있는 유저 정보들을 출력
    // chat-dev4 : /LIST all - 서버에 활성화된 채팅채널 목록을 출력함
    // chat-dev4 : /JOIN 채널방이름 - 서버에 활성화된 채팅 채널방으로 이동함
    // chat-dev5 : 각 명령어마다 다른 색으로 ANSI 컬러 적용 후 출력 및 RESET 하도록 함
    // - ADD, RM : COLOR_BLUE 후 RESET
    // - LEAVE, JOIN : COLOR_GREEN 후 RESET
    // - USER, LIST : COLOR_MAGENTA 후 RESET 
    else if(strcmp(ch, "ADD") == 0 || strcmp(ch, "RM") == 0){
        // 메시지 출력
        printf(COLOR_CYAN "\n%s\n" COLOR_RESET, str);
        fflush(stdout);  // 입력줄 깨지지 않도록
    } else if(strcmp(ch, "LEAVE") == 0 || strcmp(ch, "JOIN") == 0){
        printf(COLOR_GREEN "\n%s\n" COLOR_RESET, str);
        fflush(stdout);  // 입력줄 깨지지 않도록
    } else if(strcmp(ch, "USER") == 0 || strcmp(ch, "LIST") == 0){
        printf(COLOR_MAGENTA "\n%s\n" COLOR_RESET, str);
        fflush(stdout);  // 입력줄 깨지지 않도록
    }
}

int main(int argc, char** argv){
    struct sockaddr_in serv_addr; // 서버 주소 구조체
    char buf[BUFSIZ]; // 메시지 버퍼

    // IP 주소 입력 체크
    if(argc < 2){
        perror("NON IP ADDRESS");
        return -1;
    }

    // 1. socket() : 클라이언트 소켓 생성 (IPv4, TCP STREAM, 0 : ipv4 TCP 기준으로 자동으로 지정되는 통신 protocol)
    if((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1){
        perror("socket()");
        return -1;
    }

    // 소켓이 접속할 주소 지정
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
	// 문자열 IP를 네트워크 바이트 순서로 변환
    // inet_pton() : 입력받은 IP 주소를 네트워크 바이트 순서(빅 엔디안)로 변환하고 serv_addr(서버 주소 구조체) 에 저장
    inet_pton(AF_INET, argv[1], &(serv_addr.sin_addr.s_addr));
    serv_addr.sin_port = htons(PORT);

    // 2. connect() : 서버에 연결 요청
    // sockfd 클라이언트 소켓이 지정된 서버 주소 구조체 정보로 연결을 시도한다.
    if(connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1){
        perror("connect()");
        close(sockfd);
        return -1;
    }

    // 1. 닉네임 설정
    while (1) {
        printf("사용할 닉네임을 입력하세요: ");
        fgets(nickname, sizeof(nickname), stdin);
        nickname[strcspn(nickname, "\n")] = 0; // 개행 문자 제거

        if (strlen(nickname) == 0) {
            printf(COLOR_RED "닉네임은 비워둘 수 없습니다.\n");
            continue;
        }
        if (strlen(nickname) >= 50){
            printf("닉네임의 제한 길이(50 바이트)를 초과하였습니다.\n");
            continue;
        }
        if(strlen(nickname) < 6){
            printf("닉네임의 길이가 6바이트(한글 2글자 미만)미만 입니다.\n");
            continue;
        }

        char request[100];
        snprintf(request, sizeof(request), "/NICK %s", nickname);
        // 서버에 닉네임 중복 검사 요청
        write(sockfd, request, strlen(request));

        char response[100];
        // 서버에서 닉네임 중복 검사 결과 반환
        int n = read(sockfd, response, sizeof(response) - 1);
        if (n <= 0) {
            printf("서버와 연결이 끊겼습니다.\n");
            return -1;
        }
        response[n] = '\0';

        if (strcmp(response, "OK") == 0) {
            printf("'%s' 닉네임으로 채팅 서버 로비에 입장했습니다.\n", nickname);
            break;
        }
        else {
            printf("중복된 닉네임 입니다.\n");
            continue;
        }
    }

    // 0625 구조 수정 : 자식 프로세스에서 부모 프로세스에 보낼 데이터를 파이프로 전송하기 위한 파이프 정의
    if(pipe(pipe_child_to_parent) == -1 || pipe(pipe_child_to_parent) == -1){
        perror("pipe_child_to_parent create : ");
        close(sockfd);
        return -1;
    }

    // 0625 구조 수정 : 자식 프로세스에서 보낼 메시지가 있다는걸 알리기 위한 시그널 등록
    register_sigaction(SIGUSR1, sigusr1_handler);
            
    // 6 단계 : 좀비 프로세스 핸들러 등록
    register_sigaction(SIGCHLD, handle_sigchld);
    
    // 로비 입장
    // chat-dev5 : 처음 채팅 서버 로비 접근 시 ANSI 컬러 적용(red)
    printf(COLOR_CYAN "--- Chatting Lobby Room ---\n" COLOR_RESET);
    printf("채팅을 입력하세요.\n \
        (명령어 모음\n\t/ADD 이름 : 채널방을 '이름' 으로 개설 요청\n\t/LEAVE lobby : 현재 있는 채널방을 나오고 로비 채널로 이동하도록 요청\n\t/RM 채널방이름 : 로비가 아닌 채널방을 없애기\n\t/USER all : 접속한 전체 유저 정보 출력\n\t/USER 채널방이름 : 해당 채널방에 있는 유저 정보 출력\n\t/LIST all : 모든 채팅 채널 리스트를 출력함\n\t/JOIN 채팅채널이름 : 입력한 채팅방에 들어가기\n\t/WHISPER 상대방이름 메시지 : 접속한 상대방에게만 메시지를 보내기\n");

    // 4 단계 : 자식 프로세스에서 수신 담당 프로세스 생성 / 부모 프로세스 : 입력 및 전송 담당
    pid_t pid = fork();

    if (pid == 0) {
        // 0625 구조 수정 : 자식: 사용자 입력 → 부모로 시그널 알림
        while (1) {
            fgets(buf, BUFSIZ, stdin);
            // 마지막 문자를 '\0'으로 변경
            int len = strlen(buf);
            if (len == 0) {
                continue;
            }
            if (len > 0) {
                buf[len - 1] = '\0';
            }

            // 종료 조건: buf가 "q" 와 정확히 일치할 때 종료
            if (strcmp(buf, "q") == 0) {
                printf("[클라이언트] 종료 요청 전송 완료. 종료합니다.\n");
                break; // break 시 pid SIGTERM 시그널 발생으로 정리
            }
            
            // chat-dev2 : 위치 이동 - pipe 로 보낼 메시지 프로토콜 생성
            char sendMsg[BUFSIZ + 12 + 50];

            // chat-dev2 : 자식 클라이언트에서 입력한 문자열이 / 로 시작하는 명령어일 경우
            if(buf[0] == '/'){
                char ch[10], str[BUFSIZ + 12 + 50];
                // stdin 으로 받은 문자열 분리
                // stdin 으로 받는 문자열 예시 1 : /NICK NICKNAME
                // 예시 2 : /MSG NICKNAME:MSG
                // chat-dev2 : 버그 수정 - 메시지에 공백이 있을 때 공백을 메시지에 포함하지 못하는 경우 수정
                // => sscanf 는 공백 포함 문자열을 담기 어렵기 때문에 strchr 과 strcpy 구조로 변경
                
                char* space = strchr(buf, ' ');
                if (space != NULL) { // 공백이 포함되어 있을 때만 동작
                    sscanf(buf, "/%s", ch);
                    strcpy(str, space + 1);  // 공백 이후 문자열 복사

                    // chat-dev2 : /add 채팅방 추가
                    // 클라이언트에서 먼저 체크 사항: 채팅방 이름 입력 여부, 채팅방 이름 글자 수 제한 충족 여부
                    if(strcmp(ch, "ADD") == 0){
                        if(strlen(str) < 6){
                            printf("채팅방 이름은 6바이트 미만(한글 2글자미만) 으로 생성할 수 없습니다.\n");
                            continue;
                        }
                        if(strlen(str) >= 100){
                            printf("채팅방 이름은 100바이트 이상 으로 생성할 수 없습니다.\n");
                            continue;
                        }
                        // pipe 에 보낼 문자열 str 그대로 (명령어 동작이므로 결합 필요없이 그대로 보냄)
                        snprintf(sendMsg, sizeof(sendMsg), "%s", buf);
                        // 0625 구조 수정 : pipe 에 서버에 보낼 문자열을 쓰고
                        write(pipe_child_to_parent[1], sendMsg, strlen(sendMsg));
                        // 0625 구조 수정 : 부모 프로세스에 보낼 문자열이 있다는 걸 시그널로 알림
                        kill(getppid(), SIGUSR1);
                        
                    } // chat-dev3 : /LEAVE 명령어 - 로비가 아닌 접속한 채팅방을 나오는 명령어
                    // chat-dev4 : /RM 명령어 - 로비가 아닌 채팅방을 지우고, 채팅방에 있던 유저들을 모두 로비로 옮김
                    // chat-dev4 : /USERS all - 현재 채팅 서버에 접속한 모든 클라이언트 유저 정보(해당 유저가 접속한 채팅방, 유저 이름) 를 출력
                    //             /USERS 채팅방이름 - 해당 채팅 채널방에 속해 있는 모든 클라이언트 유저 정보를 출력
                    // chat-dev4 : /LIST all - 모든 채널방 리스트를 출력함
                    // chat-dev4 : /JOIN 채널방이름 - 서버에 활성화된 채팅 채널방으로 이동함
                    
                    else if (strcmp(ch, "LEAVE") == 0 || strcmp(ch, "RM") == 0 || strcmp(ch, "USER") == 0 || strcmp(ch, "LIST") == 0 || strcmp(ch, "JOIN") == 0){
                        // pipe 에 작성할 문자열 작성
                        snprintf(sendMsg, sizeof(sendMsg), "%s", buf);
                        write(pipe_child_to_parent[1], sendMsg, strlen(sendMsg));
                        kill(getppid(), SIGUSR1);
                    } else if(strcmp(ch, "WHISPER") == 0){
                        // chat-dev5 : /WHISPER 사용자이름 메시지 - 서버에 접속한 사용자에게만 귓속말 전달
                        snprintf(sendMsg, sizeof(sendMsg), "/WHISPER %s:%s", nickname, str);
                        write(pipe_child_to_parent[1], sendMsg, strlen(sendMsg));
                        kill(getppid(), SIGUSR1);
                    }
                } else { // / 명령어 동작을 잘못했을 경우 예외 처리(클라이언트)
                        printf("명령어 동작 방법을 확인하고 다시 입력해주세요.\n");
                        continue;
                }
            } else { // chat-dev2 : 자식 클라이언트에서 입력한 문자열이 명령어가 아닐 경우 
                // 현재 채팅방에 전송할 메시지로 동작함 (/MSG 로 동작)
                // pipe 에 보낼 문자열 결합
                snprintf(sendMsg, sizeof(sendMsg), "/MSG %s:%s", nickname, buf);
                write(pipe_child_to_parent[1], sendMsg, strlen(sendMsg));
                kill(getppid(), SIGUSR1);
            } 
        }
        kill(pid, SIGTERM); // 자식 프로세스 종료
    } else {
        // 0625 구조 수정 : 부모 : 자식으로부터 시그널을 받고 메시지를 프로토콜 전송 or 서버로부터 메시지를 받음 
        while (1) {
            // read 파트를 위한 부분 시작 : 서버로부터 메시지를 받고 process_server_message 처리에 따른 동작
            int n = read(sockfd, buf, BUFSIZ + 10 + 50);
            // 서버가 연결을 종료했거나 오류 발생 시
            // 0 : 서버에서 연결이 종료될 때 반환되는 EOF(EndOfFile)
            // -1 : 오류 발생
            if (n <= 0) {
                printf("\n[서버 연결 종료]\n");
                kill(getppid(), SIGTERM); // 부모에게 종료 알림
                exit(0);
            } else {
                buf[n] = '\0';
                process_server_message(buf);
            }

            // write part 를 위한 부분 시작 : 자식으로부터 시그널을 받고 서버에 메시지 전달
            // 부모가 읽는 파이프를 non-blocking 모드로 설정해 핸들러가 멈추지 않도록 함
            int flags = fcntl(pipe_child_to_parent[0], F_GETFL, 0);
            fcntl(pipe_child_to_parent[0], F_SETFL, flags | O_NONBLOCK);
        }
    }

    // 종료 처리
    close(sockfd);
    printf("클라이언트를 종료합니다.\n");
    return 0;
}

