CC=gcc 
CFLAGS=

mptcp_client : mptcp_client.c mptcp_client.o
	${CC} ${CFLAGS} -o mptcp_client mptcp_client.c -L../project2 -lmptcp -lpthread

clean:
	rm -f mptcp_client mptcp_client.o

run: mptcp_client
	./mptcp_client
