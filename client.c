#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>

#define PORT    5101

int sockfd; // 소켓 파일 디스크립터
char nickname[51]; // 닉네임

// 0625 구조 수정 : 전역 변수로 자식 -> 부모 데이터 파이프 선언
int pipe_child_to_parent[2];

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

    // client 자식으로부터 받은 버퍼 문자열 분리
    sscanf(buf, "/%s %s", ch, str);
    if(ch == NULL){
        return;
    }
    if(strcmp(ch, "MSG") == 0){
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
            snprintf(sendMsg, sizeof(sendMsg), "/MSG %s:%s", nickname, msg);
            
            // 파이프에 있는 문자열을 서버로 보냄
            write(sockfd, sendMsg, strlen(sendMsg));
        } 
    }
    // 수신받아서 클라이언트에서 동작할 수 있는 기능 추가 예정

}

// 서버로부터 메시지를 받아 파싱하고 출력하는 함수
// 부모 read : 메시지 파싱 후 동작
void process_server_message(char *buf) {
    // command 동작
    char ch[10];
    char str[BUFSIZ];

    // 서버로부터 받은 버퍼 문자열 분리
    sscanf(buf, "/%s %s", ch, str);

    if (ch == NULL) {
        return;
    }

    if (strcmp(ch, "MSG") == 0) {
        char nickName[51];
        char msg[BUFSIZ];

        // 채팅 메시지는 닉네임과 채팅(msg) 를 분리
        // 클라이언트로부터 받은 채팅 메시지 분리
        char* colon = strchr(str, ':');
        if (colon != NULL) {
            *colon = '\0'; // ':'를 문자열 종료로 바꿈
            strcpy(nickName, str);
            strcpy(msg, colon + 1);

            // 메시지 출력
            printf("\n[%s] >>> %s\n", nickName, msg);
            fflush(stdout);  // 입력줄 깨지지 않도록
        } 
    }
    // 여기에 다른 서버 응답(/list 응답 등) 처리 로직 추가
    
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
            printf("닉네임은 비워둘 수 없습니다.\n");
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
    printf("--- Lobby ---\n");
    printf("채팅을 입력하세요. (명령어 : )\n");

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
            // '/leave', '/q' 등 클라이언트 측 명령어 처리
            
            // pipe 로 보낼 메시지 프로토콜 생성
            char sendMsg[BUFSIZ + 12 + 50];
            // pipe 에 보낼 문자열 결합
            snprintf(sendMsg, sizeof(sendMsg), "/MSG %s:%s", nickname, buf);
            
            // 0625 구조 수정 : pipe 에 서버에 보낼 문자열을 쓰고
            write(pipe_child_to_parent[1], sendMsg, strlen(sendMsg));
            // 0625 구조 수정 : 부모 프로세스에 보낼 문자열이 있다는 걸 시그널로 알림
            kill(getppid(), SIGUSR1);
        }
        kill(pid, SIGTERM); // 자식 프로세스 종료
    } else {
        // 0625 구조 수정 : 부모 : 자식으로부터 시그널을 받고 메시지를 프로토콜 전송 or 서버로부터 메시지를 받음 
        // -> FIX : 자식 프로세스에서 
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

