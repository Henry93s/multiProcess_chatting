# 사용할 컴파일러
CC = gcc

# 컴파일 옵션 (-Wall: 경고 표시, -g: 디버깅 정보 포함)
CFLAGS = -Wall -g

# 빌드할 대상 실행 파일 이름
TARGETS = server client

# 기본 동작: server와 client 빌드
all: $(TARGETS)

# server 빌드 규칙
server: server.c
	$(CC) $(CFLAGS) -o server server.c

# client 빌드 규칙
client: client.c
	$(CC) $(CFLAGS) -o client client.c

# 빌드 결과물 제거
clean:
	rm -f $(TARGETS)