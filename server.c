#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT    5100
#define PENDING_CONN 5

int main(int argc, char** argv){
    // listen_fd : 서버에서 클라이언트 연결 요청을 기다리는 소켓
    // conn_fd : accept 를 통해 클라이언트 요청을 받고, 해당 클라이언트와 통신할 수 있는 새 소켓
    int listen_fd, conn_fd;
    struct sockaddr_in serv_addr, cli_addr; // 서버와 클라이언트 소켓 주소가 담긴 기본 구조체
    socklen_t cli_len; // 클라이언트 주소의 크기를 저장
    char buf[BUFSIZ]; // 메시지 버퍼

    // 1. socket() 
    // TCP 소켓 생성(ipv4 domain, TCP 이므로 STREAM type, 0 : ipv4 TCP 기준으로 자동으로 지정되는 통신 protocol)
    if((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1){
        perror("socket()");
        return -1;
    }

    // 2. 서버 주소 구조체 설정(serv_addr)
    memset(&serv_addr, 0, sizeof(serv_addr)); // 서버 주소 구조체 초기화
    serv_addr.sin_family = AF_INET; // ipv4
    // htonl(long), htons(short) : 네트워크 통신에서 데이터를 보내기 전에 표준에 따라 네트워크 바이트 순서(빅 엔디안)으로 변환
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY); // 서버로 요청오는 모든 ip 허용
    serv_addr.sin_port = htons(PORT); // 5100 PORT 적용

    // 3. bind()
    // 서버에서 요청을 기다리는 소켓에 서버 주소 구조체를 할당(바인딩)
    if(bind(listen_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1){
        perror("bind()");
        close(listen_fd);
        return -1;
    }

    // 4. listen()
    // 클라이언트의 접속을 대기(Pending queue 의 크기는 5)
    if(listen(listen_fd, PENDING_CONN) == -1){
        perror("listen()");
        close(listen_fd);
        return -1;
    }

    printf("서버가 %d번 포트에서 대기하고 있습니다......\n", PORT);

    while(1){ 
        cli_len = sizeof(cli_addr);

        // 5. accept()
        // listen_fd 로 클라이언트 연결 요청이 들어올 때 cli_addr(클라이언트 주소 정보) 에 클라이언트 소켓 정보를 담는다
        // conn_fd : 클라이언트와 연결된 소켓의 파일 디스크립터
        conn_fd = accept(listen_fd, (struct sockaddr*)&cli_addr, &cli_len);

        // 6. fork() 로 자식 프로세스 생성
        pid_t pid = fork();
        if(pid < 0){
            // 에러 처리
            perror("fork()");
            close(conn_fd);
            continue;
        } else if(pid == 0){
            // 자식 프로세스 일 때 처리
            close(listen_fd); // 자식 프로세스는 listen_fd 소켓을 쓰지 않음

            // inet_ntoa() : 네트워크 통신에서 사용된 클라이언트 주소인 네트워크 바이트 순서(빅 엔디안)를 . 으로 구분된 IP 주소 문자열로 변경 
            printf("새 클라이언트 연결: %s\n", inet_ntoa(cli_addr.sin_addr));

            // 7. 클라이언트로부터 메시지를 받고 다시 전송 (echo 버전)
            int n;
            // read() : 클라이언트와 연결된 소켓의 conn_fd 로부터 데이터를 읽고 buf 에 담음(읽은 바이트 수인 n 반환)
            while((n = read(conn_fd, buf, BUFSIZ)) > 0){
                write(conn_fd, buf, n);  // 담은 데이터 버퍼 buf 를 읽은 바이트 수 n 만큼, 즉 모두 그대로 클라이언트 소켓 conn_fd 에 write
            }

            printf("클라이언트 종료\n");
            close(conn_fd);
            exit(0); // 자식 프로세스 종료
        } else {
            // 부모 프로세스일 때 처리
            close(conn_fd); // 자식이 종료될 때 부모 프로세스에서도 연결됐던 소켓을 닫는다
        }
    }

    close(listen_fd); // 연결 요청 소켓을 닫음
    return 0;
}
