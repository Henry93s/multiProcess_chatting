#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>       // 4단계: 시그널 처리용
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>

#define PORT    5100
#define PENDING_CONN 5
#define MAX_CLIENTS 10 // 최대 클라이언트 수 10

// 3 -> 4단계: 전역 변수로 pipe, conn_sock, child_pid 정의
int pipe_parent_to_child[MAX_CLIENTS][2]; // 부모 → 자식
int pipe_child_to_parent[MAX_CLIENTS][2]; // 자식 → 부모
pid_t child_pid[MAX_CLIENTS];
int client_index = 0;
int client_sock[MAX_CLIENTS]; // 자식 프로세스에서 쓸 소켓 (지역 변수로도 사용 가능)
int child_index = -1; // 자식 프로세스 전용 인덱스


// 4단계: SIGUSR1, SIGUSR2 핸들러 함수 
// 부모 시그널 핸들러 SIGUSR1 : 자식이 부모에게 메시지를 보냈음을 알림 (→ 부모가 읽음)
void sigusr1_handler(int signo) {
    printf("\n[부모] SIGUSR1 핸들러 발생");
    fflush(stdout);

    char buf[BUFSIZ];
    int n;

    // 4단계: SIGUSR2로 브로드캐스트
    // i : 송신자 client index
    for (int i = 0; i < client_index; i++) {
        // 여러 메시지가 들어왔을 수 있으므로 반복
        while (1) {
            memset(buf, 0, BUFSIZ); // 버퍼를 0 으로 초기화
            // non-blocking 으로 읽기 시도
            n = read(pipe_child_to_parent[i][0], buf, BUFSIZ);
            if (n <= 0) {
                break; // 더 이상 읽을 게 없으면 break 하지 않고, 다른 파이프가 있으면 읽어야 하므로 continue 로 동작해야 함(수정)
            }

            // 메시지를 보낸 첫 자식을 찾았을 때 처리
            buf[n] = '\0'; // 문자열 끝 처리
            printf("\n[[부모] SIGUSR1 핸들러: 자식 %d 로부터 메시지 수신]: %s", i, buf);
            fflush(stdout);

            // j : 수신자 client index
            for (int j = 0; j < client_index; j++) {
                // child_pid 가 0 보다 큰지 확인하여 이미 종료된 자식에게는 보내지 않도록 함
                if(child_pid[j] > 0){
                    char signalBuf[BUFSIZ + 32];
                    // client_index 정수를 문자열 buf 와 합친 signalBuf
                    snprintf(signalBuf, sizeof(signalBuf), "%d : %s", i, buf);
                    int len = strlen(signalBuf); // 합쳐진 signalBuf 의 문자열 길이 계산
                    write(pipe_parent_to_child[j][1], signalBuf, len); // 부모 → 자식
                    kill(child_pid[j], SIGUSR2); // 부모가 자식에 알리는 시그널은 SIGUSR2 로 시그널 분리
                } else {
                    continue;
                }
            }
        }
    }
}
// 자식 시그널 핸들러 SIGUSR2 : 부모가 자식에게 브로드캐스트 알림 (→ 자식이 읽고 클라이언트에 write)
void child_sigusr2_handler(int signo) {
    printf("\n[자식] SIGUSR2 핸들러 발생");
    fflush(stdout);

    char buf[BUFSIZ];
    // 파이프가 빌 때까지 계속 읽어서 클라이언트에게 전송
    while(1){
        memset(buf, 0, BUFSIZ); // 버퍼를 0 으로 초기화
        int n = read(pipe_parent_to_child[child_index][0], buf, BUFSIZ);
        if(n <= 0){ // 읽을 데이터가 없거나(n=0 또는 n=-1), 에러 발생 시 루프 종료
            break;
        }
        if (n > 0) {
            buf[n] = '\0'; // 문자열 끝 처리
            write(client_sock[child_index], buf, n); // 클라이언트에게 전송
        }
    }
}

// 5단계 : 좀비 프로세스 회수용
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


int main(int argc, char** argv) {
    // 4단계: 부모에서 시그널 핸들러 SIGUSR1 등록
    register_sigaction(SIGUSR1, sigusr1_handler); 
    // 5단계 : 좀비 프로세스 방지
    register_sigaction(SIGCHLD, handle_sigchld); 

    // listen_fd : 서버가 클라이언트 연결을 기다리기 위한 소켓(서버 대기용) -> main() 함수 내 while(1) 내내 유지됨
    // conn_fd : 클라이언트와 연결이 성공된 직후 사용되는 소켓 -> accept 성공 후 생성되고 자식에 넘기고 부모는 닫음
    int listen_fd, conn_fd;
    struct sockaddr_in serv_addr, cli_addr;

    // 1 단계 : TCP 소켓 생성(socket())
    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket()");
        return -1;
    }

    // 1 단계 : 서버 주소 구조체 설정(memset 후 server 주소 구조체 설정)
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(PORT);

    // 1 단계 : 소켓에 서버 주소 바인딩(bind())
    if (bind(listen_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1) {
        perror("bind()");
        close(listen_fd);
        return -1;
    }

    // 1 단계 : 클라이언트 연결 대기(listen())
    if (listen(listen_fd, PENDING_CONN) < 0) {
        perror("listen()");
        close(listen_fd);
        return -1;
    }

    printf("서버가 %d 번 포트에서 대기하고 있습니다......\n", PORT);

    while (1) {
        if (client_index >= MAX_CLIENTS) {
            printf("최대 클라이언트 수 10을 초과할 수 없습니다.\n");
            break;
        }

        // 2 단계 : 클라이언트 연결 수락(accept())
        socklen_t cli_len = sizeof(cli_addr);
        conn_fd = accept(listen_fd, (struct sockaddr*)&cli_addr, &cli_len);
        if (conn_fd < 0) {
            perror("accept()");
            continue;
        }

        printf("클라이언트 연결됨: %s\n", inet_ntoa(cli_addr.sin_addr));

        // 3 -> 4단계: pipe 생성 (자식마다)
        if (pipe(pipe_child_to_parent[client_index]) == -1 ||
            pipe(pipe_parent_to_child[client_index]) == -1) {
            perror("pipe()");
            close(conn_fd);
            continue;
        }

        // 3 단계 : 자식 프로세스 생성(fork())
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork()");
            close(conn_fd);
            continue;
        } else if (pid == 0) {
            // 자식 프로세스가 client_index 를 읽을 시점에 부모 프로세스가 client_index++ 을 하지 않았다면 잘못된 값을 전달할 수 있음
            // -> 지역 변수로 복사해서 넘김
            int my_index = client_index;
            child_index = my_index; // 자식 전용 인덱스 설정
            // 자식 프로세스가 자신이 사용할 클라이언트 소켓을 저장하는 소켓 배열
            client_sock[child_index] = conn_fd; 
            
            printf("child_index : %d, client_index : %d\n", child_index, client_index);

            // 자식이 읽는 파이프를 non-blocking 으로 설정 (부모 write 가 막히지 않도록 하기 위함)
            int flags = fcntl(pipe_parent_to_child[child_index][0], F_GETFL, 0);
            fcntl(pipe_parent_to_child[child_index][0], F_SETFL, flags | O_NONBLOCK);

            // 4단계: 자식에서 시그널 핸들러 SIGUSR1 등록
            register_sigaction(SIGUSR2, child_sigusr2_handler); 

            int n;
            char buf[BUFSIZ];
            memset(buf, 0, BUFSIZ); // 버퍼를 0 으로 초기화
            while ((n = read(conn_fd, buf, BUFSIZ)) > 0) {
                // 종료 조건 : 'q' 로 메시지가 입력되면 자식 종료
                if (strcmp(buf, "q") == 0) {
                    printf("\n[자식 %d] 종료 요청 수신. 종료합니다.", getpid());
                    fflush(stdout);
                    close(conn_fd);
                    break;
                }

                // 자식 → 부모 전송
                buf[n] = '\0'; // 문자열 끝 처리
                write(pipe_child_to_parent[child_index][1], buf, n); // 3->4단계: 자식 → 부모로 write
                printf("\n[자식 %d, pid : %d] 서버에 메시지 전송: %s", child_index, getpid(), buf);
                fflush(stdout);
                // kill(getppid(), SIGUSR1); // 4단계: 부모에게 알림

                printf("\n[자식 %d] getppid() = %d\n", child_index, getppid());
                fflush(stdout);
                int kill_result = kill(getppid(), SIGUSR1);
                if (kill_result == -1) perror("kill() 실패");

            }
        } else {
            // 부모 프로세스일 때의 처리

            // 부모는 자식과의 쓰기 파이프는 닫고 읽기만 유지
            close(pipe_child_to_parent[client_index][1]);  // 부모는 read만
            close(pipe_parent_to_child[client_index][0]);  // 부모는 write만 유지

            // 부모가 읽는 파이프를 non-blocking 모드로 설정해 핸들러가 멈추지 않도록 함
            int flags = fcntl(pipe_child_to_parent[client_index][0], F_GETFL, 0);
            fcntl(pipe_child_to_parent[client_index][0], F_SETFL, flags | O_NONBLOCK);

            child_pid[client_index] = pid; // 자식 PID 저장
            client_index++;
        }
    }

    close(listen_fd);
    return 0;
}
