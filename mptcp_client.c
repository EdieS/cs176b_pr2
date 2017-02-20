#include "mptcp.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>


#define BUFFER_SIZE 4096
/*
struct pkt_data{

	int *file_data;
	int * data_pointer;
	pthread_mutex_t lock;
};

struct flow_cont{
	int *seq_num;
	int * acq_list;
	pthread_mutex_t lock;
};
*/

struct pkts{
	
	char * pkt_start;
	int seq_num;
	int acq_rev;
	int size;
	//pthread_mutex_t lock;
};

struct pkts_queue{
	
	struct pkts *pkts_list;//array of packets
	int next_pkt;
	int pkts_cnt;
	pthread_mutex_t lock;
	pthread_cond_t rwin_limit;
	//pthread_cond_t empty;
	//more to come
};

void error(const char *msg)
{
    perror(msg);
    exit(0);
}


struct pkts* init_pkts(char * filename, int * file_s, int * n_pkts){
	
	struct pkts * pkts;

	int file_desc;
	char * data_buf;
	int last_pkt_size;
	int num_pkts;
	int num_bytes;

	//file openning 
	file_desc=open(filename,O_RDWR,0);
	if (file_desc<0)
	{
		error("Error Openning file\n");
	}
	
	//read the whole file to a buffer. instead of reading chucks in each thread
	//read the file into a buffer size of 2048bytes
	
	//data_buf needs to be malloc
	data_buf=(char*)malloc(sizeof(char)*BUFFER_SIZE);
	num_bytes=read(file_desc,data_buf,BUFFER_SIZE);
	if(num_bytes==0){
		printf("complete file read\n");
	}
	else if(num_bytes<0){
		error("error reading the file\n");
	}

	*file_s=strlen(data_buf);
	printf("file_size%d\n",*file_s);

	//now data buffer has the string containing the whole file
	last_pkt_size=strlen(data_buf)% MSS;
	if (last_pkt_size){
		num_pkts=(strlen(data_buf)/MSS)+1;
	}else{
		num_pkts=(strlen(data_buf)/MSS);
	}
		
	printf("file_size%d\n",num_pkts);

	pkts=malloc(sizeof(struct pkts)*num_pkts);

	for (int i=0;i<num_pkts;i++){

		pkts[i].pkt_start=&data_buf[i*MSS];
		pkts[i].seq_num=i*MSS+1;
		pkts[i].acq_rev=0;
		if (i==num_pkts){
			if (last_pkt_size){
				pkts[i].size=last_pkt_size;
			}else
				pkts[i].size=MSS;
		}
		else{
			pkts[i].size=MSS;
		}
		
	}
	*n_pkts=num_pkts;
	return (pkts);
}

struct pkts_queue* init_data_queue(struct pkts * pkts_q){
	struct pkts_queue * pkts_queuel;

	pkts_queuel=malloc(sizeof(struct pkts_queue));
	pkts_queuel->pkts_list=(struct pkts *)pkts_q;
	pkts_queuel->next_pkt=0;
	pkts_queuel->pkts_cnt=0;

	pthread_mutex_init(&pkts_queuel->lock,NULL);
	pthread_cond_init(&pkts_queuel->rwin_limit,NULL);

	return (pkts_queuel);
}

void free_pkts_queue(struct pkts_queue * pkts_q){
	free(pkts_q->pkts_list);
}

struct mp_arg
{
	int sock_id;
	int tid;
	struct sockaddr_in serv_addr;
	int serv_port;
	int file_size;
	int num_pkts;
	struct pkts_queue * pkts_q_shared;
	
	//struct pkt_data * pkt_data;
	//struct flow_cont * flow_cont;
};



void *mp_send_recv(void *arg)
{
	struct mp_arg * my_arg=(struct mp_arg*)(arg);
	
	struct pkts_queue * pkts_qt;
	
	struct mptcp_header mp_header;
	struct mptcp_header header_recv;
	struct packet mp_pkt;
	struct packet mp_pkt_rcv;
	
	struct pkts my_pkt;
	struct pkts recv_pkt;
	struct pkts rt_pkt;

	struct sockaddr_in serv_addr;
	struct sockaddr_in client_addr;
	socklen_t clilen;
	struct timeval tval;
	
	int cwin =1;
	int s;
	int num_bytes;
	int sock_id;
	int port;
	int pkt_cnt=0;
	int pkt_index;
	int recv_timeout=0;
	int pkt_num_sent;
	int num_pkts;
	char recv_data[BUFFER_SIZE];
	int id;
	id=my_arg->tid;
	mp_pkt_rcv.data=recv_data;
	mp_pkt_rcv.header=&header_recv;
	
	//unmashaling
	pkts_qt=my_arg->pkts_q_shared;//packet queue has packet list and next packet shared variable	
	sock_id=my_arg->sock_id;
	serv_addr=my_arg->serv_addr;
	port=my_arg->serv_port;
	num_pkts=my_arg->num_pkts;

	serv_addr.sin_port=htons(port);
	if (mp_connect(sock_id,(struct sockaddr *) &serv_addr,sizeof(serv_addr)) < 0) 
		error("Internal error\n");

	//then use getsockname to get the socket for the client thread 
	clilen = sizeof(struct sockaddr);
	s=getsockname(sock_id,(struct sockaddr*) &client_addr,&clilen);
	if (s<0){
		error("getsockname failed\n");
	}

	//set the timeout value for the socket. when timout expires datagram will be resend
	tval.tv_sec = 1;
	tval.tv_usec = 0;//100ms
	if (setsockopt(sock_id, SOL_SOCKET, SO_RCVTIMEO,&tval,sizeof(tval)) < 0) {
    		error("Error\n");
	}

	mp_header.dest_addr=serv_addr;
	mp_header.src_addr=client_addr;
	mp_header.ack_num=0;
	mp_header.total_bytes=my_arg->file_size;
	
	//create a header and send the packet
	//do i need a local queue and buffer data locally in the each thread
	while(1){
 		//send multiple packets and dont wait for acks
 		//have to make sure last packet is sent then dont send
 		while(cwin>0 && pkt_index!=num_pkts-1){
			pthread_mutex_lock(&(pkts_qt->lock));
			pkt_index=pkts_qt->next_pkt;
			if (pkt_index==num_pkts-1){
				pthread_mutex_unlock(&(pkts_qt->lock));
				continue;
			}
			//check if the rwin limit is reached
			//if (((pkt_index+1)%15)==0){
			//	printf("test\n\n");
				//pthread_cond_wait(&(pkts_qt->rwin_limit),&(pkts_qt->lock));
			//	break;
			//}
			if (pkts_qt->pkts_cnt==15){
				printf("pkt_cnt_exceeded id:%d\n",id);
				break;
			}
			my_pkt=pkts_qt->pkts_list[pkt_index];
			pkts_qt->next_pkt++;
			pkts_qt->pkts_cnt++;
			pthread_mutex_unlock(&(pkts_qt->lock));

			mp_header.seq_num=my_pkt.seq_num;
			mp_pkt.header=&mp_header;
			mp_pkt.data=my_pkt.pkt_start;

			num_bytes=mp_send(sock_id,&mp_pkt, my_pkt.size,0);//header plus the pyaload???
			if (num_bytes<0){
				error("Cannot send data\n");
			}
			printf("seq_num:%d\tpacket sent:%d\tid:%d\n",my_pkt.seq_num,pkt_index,id);
			cwin--;
			pkt_cnt++;

		}
		

		num_bytes=mp_recv(sock_id,&mp_pkt_rcv,BUFFER_SIZE,0);
		if (num_bytes<0){
			printf("time out\n");
			recv_timeout=1;
			//error("Cannot receive data\n");
		}



		if (!recv_timeout){
			//end of transmission
			if (header_recv.ack_num==-1){
				printf("time out-pkts sent:%d\tid:%d",pkt_cnt,id);
				//exit(0);
			}
			else {
				//find out which packet the receiver is acking and increase the cwin if it is not a duplicate ack
				pkt_num_sent=(header_recv.ack_num /MSS);
				//if the ack received is for RWIN limit condition signal
				/*
				if (((pkt_num_sent+1)%15)==0){
					printf("test2\n\n");
					pthread_mutex_lock(&(pkts_qt->lock));
					//pthread_cond_signal(&(pkts_qt->rwin_limit));
					//pkts_qt->next_pkt++;
					pkts_qt->pkts_cnt--;
					pthread_mutex_unlock(&(pkts_qt->lock));
				}*/
				
				//if the mp_recv does not time out 
				printf("ack received:%d\tpkt_num:%d\tid:%d\t\n",header_recv.ack_num, pkt_num_sent,id);
				
				//updata the pkt queue.dont need to do this automically
				
				pthread_mutex_lock(&(pkts_qt->lock));
				pkts_qt->pkts_list[pkt_num_sent].acq_rev++;
				//pthread_cond_signal(&(pkts_qt->rwin_limit));
				//pkts_qt->next_pkt++;
				
				if (pkts_qt->pkts_list[pkt_num_sent].acq_rev==3){
					//retransmit
					pkts_qt->pkts_list[pkt_num_sent].acq_rev=0;
					rt_pkt=pkts_qt->pkts_list[pkt_num_sent];
					//pthread_mutex_unlock(&(pkts_qt->lock));
					printf("retransmit pkt sec:%d\tid:%d\n",rt_pkt.seq_num,id);
					mp_header.seq_num=rt_pkt.seq_num;
					mp_pkt.header=&mp_header;
					mp_pkt.data=rt_pkt.pkt_start;
					num_bytes=mp_send(sock_id,&mp_pkt, rt_pkt.size,0);
					//continue;
				}
				else{
					(pkts_qt->pkts_cnt)--;
				}
				
				pthread_mutex_unlock(&(pkts_qt->lock));
				cwin++;
				pkt_cnt--;
			}
		}
		else{
			//if recv times out alot then decrease the cwin for this thread and increse the another one
			//keep a time out count

		}



		}

	}

/*
void *RecThread(void *arg)
{

}
*/

//mptcp [ -n num_interfaces ] [ -h hostname ] [ -p port ] [ -f filename ]

/*
public IP:
 
128.111.68.197
 
port:
 
5176
*/
#define ARGS "n:h:p:f"
int main(int argc, char *argv[])
{
	int c;
	int lports;
	int sock_1;
	int n_paths;
	int ser_port;
	char ser_addr_str[BUFFER_SIZE];
	char file_name_str[BUFFER_SIZE];

	struct hostent *server;
	struct sockaddr_in serv_addr;
	struct sockaddr_in client_addr;
	socklen_t clilen;

	int s;
	struct addrinfo hints;
	struct addrinfo *result, *rp;

	struct mptcp_header init_mp;
	struct packet send_pkt;
	struct packet recv_pkt;

	char send_str[BUFFER_SIZE];
	char recv_str[BUFFER_SIZE];

	int *ports;
	char str_port[BUFFER_SIZE];

	pthread_t * mp_ids;
	struct mp_arg * mps;

	struct pkts * pkts_arry;//packet queue with info includded
	struct pkts_queue *pkts_q;//packet queue with pkts list and shared variables
	
	//int file_desc;
	int file_size;
	int num_pkts;
	int err;
	while((c = getopt(argc,argv,"n:h:p:f:")) != -1) {
		switch(c) {
			case 'n':
				n_paths=atoi(optarg);
				break;
			case 'h':
				strcpy(ser_addr_str,optarg);
				break;
			case 'p':
				ser_port=atoi(optarg);
				break;
			case 'f':
				strcpy(file_name_str,optarg);
				break;
			//case 'q':
			//	lports=atoi(optarg);//number of local ports
			//	printf("test\n");
			//	break;	
			default:
				//break;
				error("invalid options\n");
				//fprintf(stderr,"unrecognized command %c\n",(char)c);
				//fprintf(stderr,"usage: %s",Usage);
				//exit(1);
		}
	}

/*
	//find the address of the local machine using geraddrinfo()
	s = getaddrinfo(NULL, NULL, &hints, &result);
	if (s != 0) {
		error("getaddrinfo faild\n");
	}
	//result is a list of addr info structs. this is the first one of the list
	rp=result;
*/
	//first establish initial connection with the server
	//sock_1 = mp_socket(rp->ai_family, SOCK_MPTCP, rp->ai_protocol);
	sock_1 = mp_socket(AF_INET, SOCK_MPTCP,0);
	if (sock_1 < 0)
		error("Internal error\n");

	bzero((char *) &serv_addr, sizeof(serv_addr));
	//convert  the server address string
	inet_pton(AF_INET,ser_addr_str,&(serv_addr.sin_addr));	
	serv_addr.sin_family = AF_INET;
	//bcopy((char *)serv_addr->h_addr, (char *)&serv_addr.sin_addr.s_addr,server->h_length);
	serv_addr.sin_port = htons(ser_port);
	
	if (mp_connect(sock_1,(struct sockaddr *) &serv_addr,sizeof(serv_addr)) < 0) 
		error("Internal error\n");
	
	//then use getsockname to get the socket that the client is using 
	clilen = sizeof(struct sockaddr);
	s=getsockname(sock_1,(struct sockaddr*) &client_addr,&clilen);
	if (s<0){
		error("getsockname failed\n");
	}

	//populate the struct
	init_mp.dest_addr=serv_addr;
	init_mp.src_addr=client_addr;
	//init_mp.src_port=client_addr.sin_port;//not required at this point
	//init_mp.dest_port=serv_addr.sin_port;
	init_mp.seq_num=1;
	init_mp.ack_num=0;
	init_mp.total_bytes=7;
	
	sprintf(send_str,"MPREQ\t%d",n_paths);
	send_pkt.header=&init_mp;
	send_pkt.data=send_str;

	//request for number of ports from the server
	mp_send(sock_1,&send_pkt, sizeof(struct packet),0);
	
	recv_pkt.header=&init_mp;
	recv_pkt.data=recv_str;

	//wait for a respond from the server and read the info about port numbers to use
	mp_recv(sock_1,&recv_pkt,BUFFER_SIZE,0);

	printf("data from server%s\n",recv_str);
	
	
	//parse the received data from the server
	ports=(int *)malloc(sizeof(int)*n_paths);
	int k=0;
	int j=0;
	
	//MPOK XXXX:XXXX:XXXXX:XXXX
	for (int i=4;i<=strlen(recv_str);i++){
		if (recv_str[i]==':' || recv_str[i]=='\0'){
			str_port[k]='\0';
			ports[j]=atoi(str_port);
			j++;
			memset(str_port,0,k+1);
			k=0;
		}
		else if (recv_str[i]!='\0'){
			str_port[k]=recv_str[i];
			k++;
		}

	}

	for(int i=0;i<n_paths;i++){
		printf("ports %d\t%d\n",i+1,ports[i]);
	}
	
	//initialize the pkt queue
	pkts_arry=init_pkts(file_name_str,&file_size,&num_pkts);

	//initialize the data queue
	pkts_q=init_data_queue(pkts_arry);
	


	mp_ids= (pthread_t *)malloc(n_paths*sizeof(pthread_t));
	if(mp_ids == NULL) {
		exit(1);
	}
	mps= (struct mp_arg *)malloc(n_paths*sizeof(struct mp_arg));
	if(mps == NULL) {
		exit(1);
	}

	//create threads for each interface
	for (int i=0;i<n_paths;i++){
		//open a connection for each port
		mps[i].sock_id=mp_socket(AF_INET, SOCK_MPTCP, 0);
		mps[i].tid=i;
		mps[i].serv_addr=serv_addr;
		mps[i].serv_port=ports[i];
		mps[i].pkts_q_shared=pkts_q;
		mps[i].file_size=file_size;
		mps[i].num_pkts=num_pkts;
		//mps[i]->pkt_data=pkt_data;
		//mps[i]->flow_cont=flow_cont;

		err = pthread_create(&mp_ids[i], NULL, mp_send_recv, (void *)&mps[i]);
		if(err != 0) {
			error("pthread create failed");
		}
	}
	
	//wait for threads to finish
	for(int i=0; i < n_paths; i++) {
	err = pthread_join(mp_ids[i],NULL);
	if(err != 0) {
		printf("send thread join %d failed\n",i);
		exit(1);
	}
	}

	for(int i=0;i<n_paths;i++){
		close(mps[i].sock_id);
	}


	//free malloc
	free(mp_ids);
	free(mps);
	free_pkts_queue(pkts_q);


}
