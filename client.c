#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <signal.h>
#include <sys/wait.h>

#define PORT    5101

int sockfd; // 소켓 파일 디스크립터
// 6 단계 : 클라이언트에서 fork된 자식(수신용) 프로세스가 종료되었을 때 부모가 기다리지 않아서 발생하는 좀비 프로세스 방지 
void handle_sigchld(int signo) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void register_sigaction(int signo, void (*handler)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    if (sigaction(signo, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
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

    // 6 단계 : 좀비 프로세스 핸들러 등록
    register_sigaction(SIGCHLD, handle_sigchld);

    // 연결 성공 메시지 출력
    printf("서버 %s:%d 에 연결되었습니다.\n메시지를 입력하세요. (q 입력 시 종료)\n", argv[1], PORT);


    // 4 단계 : 자식 프로세스에서 수신 담당 프로세스 생성 / 부모 프로세스 : 입력 및 전송 담당
    pid_t pid = fork();

    if (pid == 0) {
        // 자식: 서버로부터 메시지 수신
        while (1) {
            int n = read(sockfd, buf, BUFSIZ);
            
            // 서버가 연결을 종료했거나 오류 발생 시
            // 0 : 서버에서 연결이 종료될 때 반환되는 EOF(EndOfFile)
            // -1 : 오류 발생
            if (n <= 0) {
                printf("\n[서버 연결 종료]\n");
                close(sockfd);
                return 0;
            }
            buf[n] = '\0';
            printf("\n[수신] >>> %s\n", buf);
            fflush(stdout);  // 입력줄 깨지지 않도록
        }
    } else {
        // 부모: 사용자 입력 → 서버 전송
        while (1) {
            fgets(buf, BUFSIZ, stdin);
            // 마지막 문자를 '\0'으로 변경
            int len = strlen(buf);
            if (len > 0) {
                buf[len - 1] = '\0';
            }

            // 종료 조건: buf가 "q" 와 정확히 일치할 때 종료
            if (strcmp(buf, "q") == 0) {
                write(sockfd, "q", 1); // 서버에 "q" 전송
                printf("[클라이언트] 종료 요청 전송 완료. 종료합니다.\n");
                close(sockfd);
                break;
            }
            write(sockfd, buf, strlen(buf));
        }
    }

    // 종료 처리
    close(sockfd);
    
    return 0;
}

