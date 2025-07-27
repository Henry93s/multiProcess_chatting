# 멀티프로세스 기반 TCP 채팅 서버 클라이언트 구현 프로젝트

![C](https://img.shields.io/badge/C-Language-blue.svg?style=for-the-badge&logo=c)
![Linux](https://img.shields.io/badge/Linux-System_Call-yellowgreen.svg?style=for-the-badge&logo=linux)
![TCP/IP](https://img.shields.io/badge/Networking-TCP/IP-orange.svg?style=for-the-badge)

리눅스 시스템 호출과 네트워크 프로그래밍의 깊이 있는 이해를 목표로 구현한 멀티프로세스 기반 TCP 채팅 서버 및 클라이언트입니다. 이 프로젝트는 멀티스레딩이나 I/O 멀티플렉싱(`epoll`, `select` 등)을 사용하지 않고, 오직 `fork()`를 이용한 프로세스 생성과 `pipe`를 이용한 프로세스 간 통신(IPC), 그리고 `signal`을 이용한 비동기 이벤트 처리만으로 다중 클라이언트 접속을 지원하는 서버를 구현하는 데 중점을 두었습니다.

## 🏛️ 시스템 아키텍처

본 시스템은 중앙 관제탑 역할을 하는 **부모 서버 프로세스**와 각 클라이언트의 통신을 전담하는 다수의 **자식 서버 프로세스**로 구성됩니다. 모든 통신과 제어는 `pipe`와 `signal`을 통해 비동기적으로 이루어집니다.

-   **서버 (부모 프로세스)**: 중앙 관제탑(Control Tower)
    -   새로운 클라이언트 연결 시 `fork()`로 자식 프로세스 생성.
    -   모든 자식 프로세스와 양방향 `pipe`로 연결하여 IPC 수행.
    -   채팅방 생성/삭제, 사용자 목록 관리 등 모든 상태 정보 관리.
    -   자식으로부터 메시지 수신(`SIGUSR1`) 후, 해당 채팅방의 모든 자식에게 브로드캐스트(`SIGUSR2`).
    -   `SIGCHLD`를 처리하여 좀비 프로세스 방지.
    -   `SIGINT`, `SIGTERM`을 처리하여 모든 자원을 정리하고 우아하게 종료(Graceful Shutdown).
    -   데몬(Daemon) 프로세스로 동작하며 모든 활동을 날짜별 로그 파일로 기록.

-   **서버 (자식 프로세스)**: 클라이언트 핸들러
    -   할당된 클라이언트와의 TCP 통신을 전담.
    -   클라이언트로부터 메시지를 수신하면 `pipe`를 통해 부모에게 전달하고 `SIGUSR1` 시그널 전송.
    -   부모로부터 브로드캐스트 메시지를 `SIGUSR2` 시그널로 수신하면, `pipe`에서 데이터를 읽어 클라이언트에게 전송.

-   **클라이언트**:
    -   **부모 프로세스**: 서버와의 네트워크 통신(수신) 담당.
    -   **자식 프로세스**: 사용자 키보드 입력(`fgets`) 처리 담당.
    -   자식(입력)은 `pipe`와 `SIGUSR1`을 통해 부모(네트워크)에게 메시지 전달을 요청.


*<p align="center">서버-클라이언트 상호작용 구조도</p>*
<img width="2057" height="586" alt="프로젝트 구조도" src="https://github.com/user-attachments/assets/69b42e7c-336c-481a-9faa-e3c55dcc1cd2" />


## ✨ 주요 기능

-   **다중 클라이언트 동시 접속**: `fork()`를 이용한 '1 Client per 1 Process' 모델.
-   **닉네임 기능**: 서버 접속 시 중복되지 않는 닉네임 설정.
-   **채팅방 관리**:
    -   `/add [방이름]`: 새로운 채팅방 생성.
    -   `/rm [방이름]`: 기존 채팅방 삭제.
    -   `/join [방이름]`: 지정한 채팅방으로 이동.
    -   `/leave`: 현재 채팅방을 떠나 로비로 이동.
    -   `/list`: 현재 생성된 모든 채팅방 목록 보기.
    -   `/users`: 현재 방 또는 전체 사용자의 목록 보기.
-   **귓속말 (1:1 메시지)**:
    -   `/whisper [상대방닉네임] [메시지]`: 특정 사용자에게만 비밀 메시지 전송.
-   **데몬 프로세스**: 서버가 백그라운드에서 독립적으로 실행되며, 모든 표준 출력/에러는 로그 파일(`logs/chattingServer_YYYYMMDD.log`)로 리디렉션.
-   **우아한 종료 (Graceful Shutdown)**: `Kill [Ss : 최상위 데몬 server 프로세스]` 시 모든 자식 프로세스와 자원을 안전하게 정리하고 종료.

## 🚀 시작하기

### 사전 요구사항
-   `gcc` 컴파일러
-   `make` 유틸리티
-   리눅스 기반 운영체제

### 빌드 및 실행

1.  **저장소 복제**
    ```bash
    git clone https://github.com/Henry93s/multiProcess_chatting.git
    cd multiprocess-chat
    ```

2.  **프로젝트 빌드**
    제공된 `Makefile`을 사용하여 서버와 클라이언트를 컴파일합니다.
    ```bash
    make
    ```

3.  **서버 실행**
    서버는 실행 즉시 데몬 프로세스로 전환되어 백그라운드에서 동작합니다.
    ```bash
    ./server
    ```
    서버 로그는 `logs/` 디렉토리에서 확인할 수 있습니다.
    ```bash
    tail -f logs/chattingServer_*.log
    ```

4.  **클라이언트 실행**
    새로운 터미널을 열고 서버의 IP 주소를 인자로 하여 클라이언트를 실행합니다.
    ```bash
    ./client 127.0.0.1
    ```

5.  **서버 종료**
    실행 중인 서버 프로세스(Ss : 최상위 데몬 프로세스) 의 PID를 찾아 `kill` 명령어로 종료합니다.
    ```bash
    ps aux | grep server
    kill [PID]
    ```

## ⚙️ 핵심 학습 목표

-   **프로세스 관리**: `fork()`를 사용한 다중 프로세스 서버 모델 구현 능력.
-   **TCP/IP 네트워크 프로그래밍**: 기본적인 소켓 API(`socket`, `bind`, `listen`, `accept`) 활용 능력.
-   **시그널 처리**: `sigaction()`을 사용한 비동기적 이벤트(자식 종료, IPC 알림) 처리 능력.
-   **프로세스 간 통신(IPC)**: `pipe`를 이용한 부모-자식 간 양방향 데이터 통신 구현 능력.
-   **시스템 자원 관리**: 좀비 프로세스 방지 및 파일 디스크립터, 동적 메모리 등 시스템 자원 해제 능력.
