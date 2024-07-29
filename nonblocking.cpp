// https://www.ibm.com/docs/en/i/7.3?topic=designs-example-nonblocking-io-select

#include <cstdio>
#include <cstdlib>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <errno.h>
#include <unistd.h>
#include <cstring>

#define SERVER_PORT 38000

int main() {

    int     i, len, rc, on = 1;
    int     listen_sd, max_sd, new_sd;
    int     desc_ready, end_server = false;
    int     close_conn;
    char    buffer[80];
    struct  sockaddr_in6    addr;
    struct  timeval         timeout;
    fd_set                  master_set, working_set;

    listen_sd = socket(AF_INET6, SOCK_STREAM, 0); // socket descriptors
    if (listen_sd < 0) {
        perror("socket() failed");
        exit(-1);
    }

    rc = setsockopt(listen_sd, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));
    if (rc < 0) {
        perror("setsockopt() failed");
        close(listen_sd);
        exit(-1);
    }

    rc = ioctl(listen_sd, FIONBIO, (char *)&on); // sets or clears non-blocking I/O. 0 = blocking is enabled, 0 != non-blocking mode is enabled
    if (rc < 0) {
        perror("ioctl() failed");
        close(listen_sd);
        exit(-1);
    }

    // 상당히 흥미로운 부분의 연속
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    memcpy(&addr.sin6_addr, &in6addr_any, sizeof(in6addr_any));
    addr.sin6_port = htons(SERVER_PORT);
    rc = bind(listen_sd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0) {
        perror("bind() failed");
        close(listen_sd);
        exit(-1);
    }

    rc = listen(listen_sd, 32);
    if (rc < 0) {
        printf("listen() failed");
        close(listen_sd);
        exit(-1);
    }

    FD_ZERO(&master_set); // file descriptor 에 설정된 set 들을 전부 지움. file descriptor 를 초기화 할 때 맨 처음으로 사용되야 함
    max_sd = listen_sd;
    FD_SET(listen_sd, &master_set); // file descriptor fd 를 set 에 추가함. set 에 이미 추가된 file descriptor 를 다시 추가하게 되면 no-op 임. 에러 안 냄
    // inner implementation 을 봐야 할 듯. 아직 잘 이해가 안 감.
    
    // timeval -> time structures (BSD 시스탬의 유틸리티들 있음)
    timeout.tv_sec = 3 * 60; // 지니간 시간 (seconds)
    timeout.tv_usec = 0; // 지나간 시간의 나머지 (nanoseconds)

    do {
        memcpy(&working_set, &master_set, sizeof(master_set));

        printf("Waiting on select()...\n");
        rc = select(max_sd + 1, &working_set, NULL, NULL, &timeout); // FD_SETSIZE (1024) 보다 작은 파일 디스크립터 넘버만 모니터링 가능함. 현대 애플리케이션은 poll 또는 epoll 사용을 권장.
        // nfds         : readfds, writefds, exceptfds 중 highest-numbered file descriptor + 1 이어야 함. 각각의 set 의 indicated 파일 디스크립터들이 limit까지 확인됬는지 용도. (but see BUGS)
        // readfds      : set 안의 파일 디스크립터가 읽기 가능한 상태인지 확인함. read 작업이 block 없이 가능한 상태를 읽기 가능한 상태라고 함. 특별히, 파일 디스크립터는 end-of-file 일때도 ready 임.
        // writefds     : set 안의 파일 디스크립터가 쓰기 가능한 상태인지 확인함. 파일 디스크립터가 읽기 기능한 상태라고 해도, 큰 쓰기 작업은 여전히 block 할 가능성이 있음.
        // exceptfds    : set 안의 파일 디스크립터가 "예외 조건"인지 확인함. 예외 조건의 예시는 poll(2) 의 POLLPRI 를 참조.
        //                select() 반환 뒤, exceptfds 는 예외 조건이 발생한 디스크립터를 제외하고 나머지 디스크립터가 제거됨.
        // timeout      : timeval 구조체, select() 가 파일 디스크립터가 ready 가 될 때 까지 얼마나 오래 block 해야 하는지 나타냄. 호출은 아레의 조건이 될 때 까지 block 함.
        //                * 파일 디스크립터가 ready 됨.
        //                * 호출이 시그널 헨들러에 의해 interrupted 되었거나,
        //                * timeout이 만료됨.
        //                timeout interval 은 system clock 으로 반올림됨. 그리고 커널 스케줄링 딜레이는 블로킹 간격이 조금 늘어날 수 있다는 것임.
        //                timeval 구조체의 두 필드가 모두 0 인 경우, select() 는 바로 반환함. (polling 할 때 유용하다고 함)
        //                timeout 이 NULL 인 경우, select() 는 파일 디스크립터가 ready 될 때 까지 무한정 block 함.
        //
        // Linux 에서, select() 는 read block 이 존재하든 말든 socket file decriptor 를 "ready for reading" 상태라고 보고할 수 있음.
        // 이 상태는 데이터가 도착했지만, 잘못된 checksum을 가져서 폐기되었을 시 나타날 수 있음. 다른 경우의 수가 더 존재할 수 있음.
        // 그래서, block 하면 안 되는 소켓에게는 O_NONBLOCK 을 사용하는 것이 안전할 수 있음.

        // Q: 가능한 상태라고 어떻게 확인하는가?
        // 생각: EFLAGS?

        if (rc < 0) { // error
            perror("    select() failed");
            break;
        }

        if (rc == 0) { // timed out
            printf("    select() timed out. End program.\n");
            break;
        }

        // 하나 이상의 디스크립터가 읽기 가능한 상태임
        desc_ready = rc;

        for (i = 0; i <= max_sd && desc_ready > 0; ++i) { // 상당히 흥미로운 조건문
            if (FD_ISSET(i, &working_set)) { // fd 가 set 에 존재하면 nonzero 반환, 아니면 zero
                desc_ready -= 1; // 하나 읽었음.

                if (i == listen_sd) { // listen 동작이 어떻게 되는 걸까
                    // 아마도 파일 디스크립터 숫자가 listen_sd 와 같은가로 확인하는듯.
                    // 그래서 FD_ISSET 에서 nonzero 반환했을때 그걸로 뭐시기뭐시기 이러쿵저러쿵
                    printf("    Listening socket is readable\n");

                    do {
                        new_sd = accept(listen_sd, NULL, NULL);
                        if (new_sd < 0) {
                            if (errno != EWOULDBLOCK) {
                                perror("    accept() failed");
                                end_server = true;
                            }
                            break;
                        }

                        printf("    New incomming connection - %d\n", new_sd);
                        FD_SET(new_sd, &master_set); // 새 커넥션 master_set 에 추가함
                        if (new_sd > max_sd) {
                            max_sd = new_sd;
                        }
                    } while (new_sd != -1);
                }
                else { // listen_sd 말고 다른 유저 커넥션 set
                    printf("    Descriptor %d is readable\n", i);
                    close_conn = false;

                    do {
                        rc = recv(i, buffer, sizeof(buffer), 0);
                        if (rc < 0) {
                            if (errno != EWOULDBLOCK) { // wouldblock 말고 다른 에러
                                perror("    recv() failed");
                                close_conn = true;
                            }
                            break;
                        }

                        if (rc == 0) { // closed
                            printf("    Connection closed\n");
                            close_conn = true;
                            break;
                        }

                        rc = send(i, buffer, sizeof(buffer), 0);
                        if (rc < 0) {
                            perror("    send() failed");
                            close_conn = true;
                            break;
                        }

                    } while (true);

                    if (close_conn) {
                        close(i);
                        FD_CLR(i, &master_set);
                        if (i == max_sd) {
                            while (FD_ISSET(max_sd, &master_set) == false) {
                                max_sd -= 1;
                            }
                        }
                    }

                } // 유저 커넥션 set (else)
            }
        }

    } while (end_server == false);


    for (i = 0; i <= max_sd; ++i) {
        if (FD_ISSET(i, &master_set)) {
            close(i);
        }
    }

    return 0;
}
