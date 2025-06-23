#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <signal.h>

#define PORT    5100

int main(int argc, char** argv){
    int sockfd; // 소켓 파일 디스크립터
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

    // 연결 성공 메시지 출력
    printf("서버 %s:%d 에 연결되었습니다.\n메시지를 입력하세요. (q 입력 시 종료)\n", argv[1], PORT);


    // 4 단계 : 자식 프로세스에서 수신 담당 프로세스 생성 / 부모 프로세스 : 입력 및 전송 담당
    pid_t pid = fork();

    if (pid == 0) {
        // 자식: 서버로부터 메시지 수신
        while (1) {
            int n = read(sockfd, buf, BUFSIZ);
            
            if (n <= 0) {
                printf("\n[서버 연결 종료]\n");
                exit(0);
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
                printf("종료합니다.\n");
                kill(pid, SIGTERM); // 자식 프로세스 종료
                break;
            }
            write(sockfd, buf, strlen(buf));
        }
    }

    // 종료 처리
    close(sockfd);
    
    return 0;
}

