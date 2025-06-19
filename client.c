#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

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

    // 입력 -> 전송 -> 응답 loop
    while(1){
        printf(">>> ");
        // 표준 입력 받기
        fgets(buf, BUFSIZ, stdin);

        // 입력이 "q" 로 시작하면 종료
        if(strncmp(buf, "q", 1) == 0) {
            break;
        }

        // 서버로 메시지 전송
        write(sockfd, buf, strlen(buf));

        // 서버로부터 응답 수신
        // read() : 서버와 연결된 소켓의 sockfd 로부터 데이터를 읽고 buf 에 담음
        int n = read(sockfd, buf, BUFSIZ);
        if(n <= 0){ 
            perror("서버 연결 종료됨\n");
            break;
        } 

        // 수신 문자열 끝 처리
        buf[n] = '\0';

        // 서버 응답 출력
        printf("서버 응답 %d : %s", sockfd, buf);
    }

    // 종료 처리
    close(sockfd);
    
    return 0;
}

