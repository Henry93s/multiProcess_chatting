#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>       // 4단계: 시그널 처리용
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>

#define PORT    5101
#define PENDING_CONN 5
#define MAX_CLIENTS 10 // 최대 클라이언트 수 10

// 3 -> 4단계: 전역 변수로 pipe, conn_sock, child_pid 정의
int pipe_parent_to_child[MAX_CLIENTS][2]; // 부모 → 자식
int pipe_child_to_parent[MAX_CLIENTS][2]; // 자식 → 부모
pid_t child_pid[MAX_CLIENTS];
int client_index = 0;
int client_sock[MAX_CLIENTS]; // 자식 프로세스에서 쓸 소켓 (지역 변수로도 사용 가능)
int child_index = -1; // 자식 프로세스 전용 인덱스
// listen_fd : 서버가 클라이언트 연결을 기다리기 위한 소켓(서버 대기용) -> main() 함수 내 while(1) 내내 유지됨
// conn_fd : 클라이언트와 연결이 성공된 직후 사용되는 소켓 -> accept 성공 후 생성되고 자식에 넘기고 부모는 닫음
// 6 단계 : graceful_shutdown 을 위해 main 지역 변수에서 전역 변수로 이동
int listen_fd, conn_fd;

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
                break; // 더 이상 읽을 게 없으면 break
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

// 5단계 : 좀비 프로세스(부모 프로세스가 종료되어도 자식의 "종료" 상태(ex. pid) 가 커널에 남아 있는 상태 - 자원을 사용하진 않음) 회수용
void handle_sigchld(int signo) {
    pid_t pid;
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0){
        // 해당 pid 가 어느 인덱스인지 찾아서 child_pid[i] = 0 처리
        for(int i = 0; i < MAX_CLIENTS; i++){
            if(child_pid[i] == pid){
                child_pid[i] = 0;
                break;
            }
        }
    }
}

// 6단계 : Graceful shutdown 핸들러 추가
// 고아 프로세스(부모 프로세스가 먼저 종료된 후 자식 프로세스가 여전히 "실행 중" 인 상태 - 실제 자원을 사용)
// -> Ctrl + C 로 종료 시에는 SIGINT, 비정상 종료 시 
void graceful_shutdown_handler(int signo) {
    printf("\n[부모] 서버 종료 시그널 수신 : 모든 자식 종료 중 ...\n");

    // 1. 자식 프로세스 종료 요청 및 대기
    for (int i = 0; i < client_index; i++) {
        if (child_pid[i] > 0) {
            kill(child_pid[i], SIGTERM); // 자식 종료 요청
        }
    }
    // 모든 자식이 종료될 때까지 기다림
    for (int i = 0; i < client_index; i++) {
        if (child_pid[i] > 0) {
            waitpid(child_pid[i], NULL, 0);
            child_pid[i] = 0; // 종료된 자식 PID 초기화
        }
    }

    // 2. 파이프 및 클라이언트 소켓 닫기
    for (int i = 0; i < client_index; i++) {
        close(pipe_child_to_parent[i][0]); // 부모 read
        close(pipe_parent_to_child[i][1]); // 부모 write
        close(client_sock[i]);             // 클라이언트 소켓
    }

    // 3. 서버 listen 소켓 닫기
    if (listen_fd > 0) {
        close(listen_fd);
        listen_fd = -1;
    }

    printf("[부모] 서버 종료 완료. 자원 회수 완료.\n");
    exit(0);
}

// 6 단계 : 자식 프로세스 쪽 sigterm handler
void child_sigterm_handler(int signo) {
    printf("\n[자식 %d] 종료 시그널 수신. 종료 중...\n", getpid());
    close(client_sock[child_index]);  // 담당 클라이언트 소켓 닫기
    close(pipe_child_to_parent[child_index][1]);
    close(pipe_parent_to_child[child_index][0]);
    exit(0);
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
    // 5단계 : 좀비 프로세스(자식이 종료된 후 PID 만 남아서 자원 누수가 발생하는 프로세스) 방지
    // -> 자식이 종료될 경우 자원을 회수하여 좀비 프로세스가 남지 않도록 함
    register_sigaction(SIGCHLD, handle_sigchld); 
    // 6단계 : 부모 프로세스 Graceful shutdown 핸들러 추가
    // 고아 프로세스(부모 프로세스가 먼저 종료된 후 자식 프로세스가 여전히 "실행 중" 인 상태 - 실제 자원을 사용)
    register_sigaction(SIGINT, graceful_shutdown_handler);
    register_sigaction(SIGTERM, graceful_shutdown_handler);

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
        // 2 단계 : 클라이언트 연결 수락(accept())
        socklen_t cli_len = sizeof(cli_addr);
        conn_fd = accept(listen_fd, (struct sockaddr*)&cli_addr, &cli_len);
        if (conn_fd < 0) {
            perror("accept()");
            continue;
        }

        // 6단계 : 새 클라이언트를 위한 빈 슬롯(인덱스) 찾기
        int new_client_idx = -1;
        for(int i = 0; i < MAX_CLIENTS; i++){
            if(child_pid[i] == 0){ // child_pid 가 0 이면 비어있는 슬롯
                new_client_idx = i; // 비어있는 슬롯에 새 클라이언트 idx 할당하기 위함
                break;
            }
        }
        // 6단계 : 빈 슬롯이 없을 때 (서버 꽉 찬 상태)
        if(new_client_idx == -1){
            printf("서버가 꽉 찼습니다. 연결을 거부합니다.\n");
            write(conn_fd, "서버가 꽉 찼습니다.\n", strlen("서버가 꽉 찼습니다.\n"));
            close(conn_fd);
            continue; // 다음 accept() 대기로
        }

        printf("클라이언트 연결됨: %s\n", inet_ntoa(cli_addr.sin_addr));

        // 3 -> 4단계: pipe 생성 (자식마다)
        // 4 -> 6단계 : 찾은 인덱스(new_client_idx)를 사용하여 파이프 생성
        if (pipe(pipe_child_to_parent[new_client_idx]) == -1 ||
            pipe(pipe_parent_to_child[new_client_idx]) == -1) {
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
            // 자식 프로세스일 때의 처리
            // 6 단계 : 자식이 sigterm 을 받을 때, 정리하기 위한 핸들러 추가
            register_sigaction(SIGTERM, child_sigterm_handler);
            register_sigaction(SIGINT, child_sigterm_handler);

            // 6 단계 : 새로 찾은 인덱스를 자신의 인덱스(자식)으로 사용
            child_index = new_client_idx; // 자식 전용 인덱스 설정
            // 자식 프로세스가 자신이 사용할 클라이언트 소켓을 저장하는 소켓 배열
            client_sock[child_index] = conn_fd; 
            
            printf("child_index : %d, new_client_idx : %d\n", child_index, new_client_idx);

            // 파이프 정리
            close(pipe_child_to_parent[child_index][0]);
            close(pipe_parent_to_child[child_index][1]);

            // 자식이 읽는 파이프를 non-blocking 으로 설정 (부모 write 가 막히지 않도록 하기 위함)
            int flags = fcntl(pipe_parent_to_child[child_index][0], F_GETFL, 0);
            fcntl(pipe_parent_to_child[child_index][0], F_SETFL, flags | O_NONBLOCK);

            // 4단계: 자식에서 시그널 핸들러 SIGUSR2 등록
            register_sigaction(SIGUSR2, child_sigusr2_handler); 

            int n;
            char buf[BUFSIZ];
            
            while (1) {
                memset(buf, 0, BUFSIZ); // 버퍼를 0 으로 초기화
                n = read(conn_fd, buf, BUFSIZ);

                // 6 단계 : read() 가 0 일 때 graceful 연결 종료 처리를 위한 부분 처리
                if (n <= 0) {
                    printf("\n[자식 %d, pid %d] 클라이언트 연결 종료 감지. 종료합니다.\n", child_index, getpid());
                    fflush(stdout);
                    close(client_sock[child_index]);
                    break;
                }

                // 종료 조건 : 'q' 로 메시지가 입력될 때 자식을 graceful 종료 처리
                if (strcmp(buf, "q") == 0) {
                    printf("\n[자식 %d] 종료 요청 수신. 종료합니다.", getpid());
                    fflush(stdout);
                    close(client_sock[child_index]); // 자식에서 종료 시 자신의 conn_fd 를 닫아야 함
                    break;
                }

                // 자식 → 부모 전송
                buf[n] = '\0'; // 문자열 끝 처리
                write(pipe_child_to_parent[child_index][1], buf, n); // 3->4단계: 자식 → 부모로 write
                printf("\n[자식 %d, pid : %d] 서버에 메시지 전송: %s", child_index, getpid(), buf);
                fflush(stdout);

                printf("\n[자식 %d] getppid() = %d\n", child_index, getppid());
                fflush(stdout);
                kill(getppid(), SIGUSR1); // 4단계: 부모에게 알림
            }

            exit(0); // 자식 프로세스 종료
        } else {
            // 부모 프로세스일 때의 처리
            // 부모는 자식에게 conn_fd 를 넘기고 자신의 copy 된 conn_fd 는 닫고, 자식에서 종료 시 자신의 conn_fd 를 닫아야 함
            close(conn_fd); // 부모에서는 conn_fd(연결 소켓) 를 사용하지 않음

            // 부모는 자식과의 쓰기 파이프는 닫고 읽기만 유지
            close(pipe_child_to_parent[new_client_idx][1]);  // 부모는 read만
            close(pipe_parent_to_child[new_client_idx][0]);  // 부모는 write만 유지

            // 부모가 읽는 파이프를 non-blocking 모드로 설정해 핸들러가 멈추지 않도록 함
            int flags = fcntl(pipe_child_to_parent[new_client_idx][0], F_GETFL, 0);
            fcntl(pipe_child_to_parent[new_client_idx][0], F_SETFL, flags | O_NONBLOCK);

            child_pid[new_client_idx] = pid; // 자식 PID 저장
            // 6단계 : client_index 를 루프의 최대 경계로 사용하기 위해 업데이트
            if (new_client_idx >= client_index) {
                client_index = new_client_idx + 1;
            }
        }
    }

    close(listen_fd);
    return 0;
}
