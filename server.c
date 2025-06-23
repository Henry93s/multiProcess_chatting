#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT    5100
#define PENDING_CONN 5
#define MAX_CLIENTS 10 // 최대 클라이언트 수 10

int main(int argc, char** argv){
    // listen_fd : 서버에서 클라이언트 연결 요청을 기다리는 소켓
    // conn_fd : accept 를 통해 클라이언트 요청을 받고, 해당 클라이언트와 통신할 수 있는 새 소켓
    int listen_fd;
    int conn_fd[MAX_CLIENTS] = {0}; // 2단계 : conn_fd[10] : 최대 클라이언트 소켓 10개
    pid_t child_pid[MAX_CLIENTS] = {0}; // 자식 프로세스 PID 저장
    struct sockaddr_in serv_addr, cli_addr; // 서버와 클라이언트 소켓 주소가 담긴 기본 구조체
    char buf[BUFSIZ]; // 메시지 버퍼

    // 1. socket() 
    // TCP 소켓 생성(ipv4 domain, TCP 이므로 STREAM type, 0 : ipv4 TCP 기준으로 자동으로 지정되는 통신 protocol)
    if((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
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
    if(listen(listen_fd, PENDING_CONN) < 0){
        perror("listen()");
        close(listen_fd);
        return -1;
    }

    printf("서버가 %d 번 포트에서 대기하고 있습니다......\n", PORT);

    
    int client_index = 0;
    // 3 단계 개발 : pipe 생성
    // pipe 가 자식마다 개별 생성되어 부모가 다수의 pipe 를 관리해야함.
    int pipe_fd[MAX_CLIENTS][2] = {0}; // 0 : 부모 read, 1 : 자식 write
    

    while(1){ 
        // 2단계 : 현재 클라이언트 수가 10개 이상일 때 break;
        if(client_index >= MAX_CLIENTS){
            printf("최대 클라이언트 수 10을 초과할 수 없습니다.\n");
            break;
        }

        // 5. accept()
        // listen_fd 로 클라이언트 연결 요청이 들어올 때 cli_addr(클라이언트 주소 정보) 에 클라이언트 소켓 정보를 담는다
        // conn_fd : 클라이언트와 연결된 소켓의 파일 디스크립터
        socklen_t cli_len = sizeof(cli_addr);
        // 2단계 : 다중 클라이언트 연결을 위해서 conn_fd 배열 client_index 에 accept 로 소켓의 파일 디스크립터를 받음
        conn_fd[client_index] = accept(listen_fd, (struct sockaddr*)&cli_addr, &cli_len);
        if(conn_fd[client_index] < 0){
            perror("accept()"); 
            continue;
        }

        // 3 단계 : pipe 생성 오류 처리
        if(pipe(pipe_fd[client_index]) == -1){
                perror("pipe()");
                return -1;
        }

        // 6. fork() 로 자식 프로세스 생성
        // fork() 시 부모 프로세스의 모든 파일 디스크립터가 자식 프로세스에도 복사됨
        pid_t pid = fork();
        if(pid < 0){
            // 에러 처리
            perror("fork()");
            close(conn_fd[client_index]);
            continue;
        } else if(pid == 0){
            // 자식 프로세스 일 때 처리
            close(listen_fd);  // 자식 프로세스에서는 닫음
            close(pipe_fd[client_index][0]); // 3단계 : 자식 프로세스에서 read 는 닫음

            // inet_ntoa() : 네트워크 통신에서 사용된 클라이언트 주소인 네트워크 바이트 순서(빅 엔디안)를 . 으로 구분된 IP 주소 문자열로 변경 
            printf("새 클라이언트 연결: %s\n", inet_ntoa(cli_addr.sin_addr));
            printf("connected: conn_fd = %d, pid = %d\n", conn_fd[client_index], getpid());
                
            // 7. 클라이언트로부터 메시지를 받고 다시 전송 (echo 버전)
            int n;
            // read() : 클라이언트와 연결된 소켓의 conn_fd 로부터 데이터를 읽고 buf 에 담음(읽은 바이트 수인 n 반환)
            // 3 단계 : 자식들이 메시지를 작성하는 pipe 를 읽기 위해 read 루프를 blocking 으로 구현
            while((n = read(conn_fd[client_index], buf, BUFSIZ)) > 0){
                write(pipe_fd[client_index][1], buf, n);  // 3단계 : 담은 데이터 버퍼 buf 를 읽은 바이트 수 n 만큼, 즉 모두 그대로 pipe 에 write
            }

            close(conn_fd[client_index]);
            // 3단계 : 자식 프로세스가 더 이상 메시지를 보내지 않을 때('q' 입력 등) 
            close(pipe_fd[client_index][1]);
            exit(0); // 자식 프로세스 정상 종료
        } else {
            // 부모 프로세스일 때 처리
            close(pipe_fd[client_index][1]); // 3단계 : 부모 프로세스에서는 write 닫음
            child_pid[client_index] = pid;

            // 2 단계 : 다중 클라이언트이므로 연결 소켓 conn_fd 배열 관리(close)
            close(conn_fd[client_index]); // 부모 프로세스에서 conn_fd close, 자식 프로세스에서는 소켓 유지
            client_index++; // 다음 클라이언트를 위해서 인덱스 증가
        }

        // 3단계 : 부모에서 pipe 읽기 (단일 읽기 시도, 다음 4단계에서 별도 루프로 반복 예정)
        for (int i = 0; i < client_index; i++) {
            int n = read(pipe_fd[i][0], buf, BUFSIZ);
            if (n > 0) {
                printf("부모 수신 메시지 (from child %d): %s\n", child_pid[i], buf);
            }
        }
    }

    close(listen_fd); // 연결 요청 소켓을 닫음
    return 0;
}
