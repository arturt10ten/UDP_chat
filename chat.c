//gcc chat.c -D_REENTERANT -rdynamic -o chat -lpthread
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/file.h>

#define max(n,m) (((m) > (n)) ? (m) : (n))

#define DEBUG_MODE 0

#define REPEAT_DELAY 500000

#define MAX_MSG_LEN 1024

#define S_TYPE_PING 1
#define S_TYPE_PONG 2
#define S_TYPE_CONNECT 3
#define S_TYPE_CLIENT 4



int log_fd=-1;

struct server_data;
struct addr{
	struct sockaddr addr;
	socklen_t len;
};

struct msg_packet{
	char type;
	char subtype;
	union{
		char msg[MAX_MSG_LEN];
		struct addr addr;
	};
};
struct client_list{
	struct client_list * next;
	struct addr addr;
};
struct f_list{
	struct f_list * next;
	void (*func)(struct server_data *,const struct msg_packet *,const struct addr *);
	char type;
	char subtype;
};
struct sf_list{
	struct sf_list * next;
	void (*func)(struct server_data *);
};
struct server_data{
	int fd;
	struct client_list * clients;
	struct client_list * to_remove;
	struct f_list * handlers;
	struct sf_list * repeaters;
	pthread_mutex_t lock;
	pthread_t server;
	pthread_t repeat;
};

int open_sock(){
	int i=socket(PF_INET,SOCK_DGRAM,0);
	return i;
}
struct addr get_client(const char *ip,int port){
	dprintf(log_fd,"get client %s:%d\n",ip,port);
	struct sockaddr_in addr;
	struct addr ret;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if(ip) addr.sin_addr.s_addr =inet_addr(ip);
	else   addr.sin_addr.s_addr =INADDR_BROADCAST;
	memcpy(&ret.addr,&addr,sizeof(struct sockaddr_in));
	ret.len=sizeof(struct sockaddr_in);
	{
		char host[NI_MAXHOST],service[NI_MAXSERV];
		getnameinfo(&ret.addr,ret.len,host,NI_MAXHOST,service,NI_MAXSERV,NI_NUMERICSERV|NI_NUMERICHOST);
		dprintf(log_fd,"get - %s:%s\n",host,service);
	}
	return ret;
}
void local_bind(int fd,int port){
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr =INADDR_ANY;
	bind(fd,(struct sockaddr *)&addr,sizeof(addr));
}
void add_client(struct server_data *server,const struct addr *client){
	struct client_list *cur=(server->clients);
	int i=1;
	while(cur){
		if(cur->addr.len==client->len)
		if(!memcmp(&(cur->addr.addr),&(client->addr),client->len))return;
		if(cur==server->to_remove)break;
		cur=cur->next;
	}
	if(cur)cur=cur->next;
	while(cur){
		if(cur->addr.len==client->len)
		if(!memcmp(&(cur->addr.addr),&(client->addr),client->len)){
			cur->addr.len=0;
			i=0;
		}
		cur=cur->next;
	}
	struct client_list *new=malloc(sizeof(struct client_list));
	new->next=server->clients;
	new->addr=*client;
	server->clients=new;
	if(i){
		char host[NI_MAXHOST],service[NI_MAXSERV];
		getnameinfo(&client->addr,client->len,host,NI_MAXHOST,service,NI_MAXSERV,NI_NUMERICSERV|NI_NUMERICHOST);
		printf("connected : %s:%s\n",host,service);
	}
}
void add_handler(struct server_data * server,void (*func)(struct server_data *,const struct msg_packet *,const struct addr *),char type,char subtype){
	struct f_list *new=malloc(sizeof(struct f_list));
	pthread_mutex_lock(&server->lock);
	new->next=server->handlers;
	new->func=func;
	new->type=type;
	new->subtype=subtype;
	server->handlers=new;
	pthread_mutex_unlock(&server->lock);
}
void add_repeater(struct server_data * server,void (*func)(struct server_data *)){
	struct sf_list *new=malloc(sizeof(struct f_list));
	pthread_mutex_lock(&server->lock);
	new->next=server->repeaters;
	new->func=func;
	server->repeaters=new;
	pthread_mutex_unlock(&server->lock);
}
void get_new_msg(struct server_data * server){
	struct addr addr;
	struct msg_packet msg;
	addr.len=sizeof(struct sockaddr_in);
	recvfrom(server->fd,&msg,sizeof(struct msg_packet),0,&addr.addr,&addr.len);
	{
		char host[NI_MAXHOST],service[NI_MAXSERV];
		getnameinfo(&addr.addr,addr.len,host,NI_MAXHOST,service,NI_MAXSERV,NI_NUMERICSERV|NI_NUMERICHOST);
		dprintf(log_fd,"recv pack from : %s:%s\n",host,service);
	}
	pthread_mutex_lock(&server->lock);
	add_client(server,&addr);
	struct f_list * f=server->handlers;
	while(f){
		if(f->type==msg.type){
			if((f->subtype==0)||(f->subtype==msg.subtype)){
				f->func(server,&msg,&addr);
			}
		}
		f=f->next;
	}
	pthread_mutex_unlock(&server->lock);
}
void main_handl_function(struct server_data * server,const struct msg_packet * msg,const struct addr * addr){
	if(msg->subtype==S_TYPE_PING){
		struct msg_packet replay;
		replay.type=0;
		replay.subtype=S_TYPE_PONG;
		memcpy(replay.msg,msg->msg,MAX_MSG_LEN);
		sendto(server->fd,&replay,sizeof(struct msg_packet),0,&addr->addr,addr->len);
	}
	if(msg->subtype==S_TYPE_CLIENT){
		struct msg_packet replay;
		replay.type=0;
		replay.subtype=S_TYPE_PING;
		memset(replay.msg,0,MAX_MSG_LEN);
		sendto(server->fd,&replay,sizeof(struct msg_packet),0,&msg->addr.addr,msg->addr.len);
	}
	if(msg->subtype==S_TYPE_CONNECT){
		struct client_list * cur=server->clients;
		while(cur){
			struct msg_packet replay;
			replay.type=0;
			replay.subtype=S_TYPE_CLIENT;
			replay.addr=cur->addr;
			sendto(server->fd,&replay,sizeof(struct msg_packet),0,&addr->addr,addr->len);
			replay.addr=*addr;
			sendto(server->fd,&replay,sizeof(struct msg_packet),0,&cur->addr.addr,cur->addr.len);
			cur=cur->next;
		}
	}
}
void connect_to_client(struct server_data * server, const struct addr * addr){
	struct msg_packet replay;
	replay.type=0;
	replay.subtype=S_TYPE_CONNECT;
	sendto(server->fd,&replay,sizeof(struct msg_packet),0,&addr->addr,addr->len);
}
void sendtoall(struct server_data * server,const struct msg_packet * msg){
	struct client_list * cur=server->clients;
	while(cur){
		sendto(server->fd,msg,sizeof(struct msg_packet),0,&cur->addr.addr,cur->addr.len);
		cur=cur->next;
	}
}
void * server_main_loop(void *in_data){
	struct server_data * server=in_data;
	while(1){
		get_new_msg(server);
	}
}
void repeat_func(struct server_data * server){
	pthread_mutex_lock(&server->lock);
	struct sf_list * f=server->repeaters;
	while(f){
		f->func(server);
		f=f->next;
	}
	pthread_mutex_unlock(&server->lock);
}
void * repeat_main_loop(void * in_data){
	struct server_data * server=in_data;
	while(1){
		repeat_func(server);
		usleep(REPEAT_DELAY);
	}
}
void * broadcast(void*in_data){
	struct server_data * server=in_data;
	while(1){
		int i;
		struct msg_packet data;
		memset(data.msg,0,MAX_MSG_LEN);
		data.type=0;
		data.subtype=S_TYPE_CONNECT;
		for(i=49152;i<65536;i++){
			struct addr ret=get_client(NULL,i);
			sendto(server->fd,&data,sizeof(struct msg_packet),0,&ret.addr,ret.len);
			//printf("try port : %d\n",i);
		}
		for(i=49151;i>1023;i--){
			struct addr ret=get_client(NULL,i);
			sendto(server->fd,&data,sizeof(struct msg_packet),0,&ret.addr,ret.len);
			//printf("try port : %d\n",i);
		}
		sleep(10);
	}
}
void clean_old_connects(struct server_data * server){
	if(server->to_remove!=NULL){
		struct client_list * cur=server->to_remove->next;
		server->to_remove->next=NULL;
		while(cur){
			struct client_list * tmp=cur;
			cur=cur->next;
			free(tmp);
			if(tmp->addr.len){
				char host[NI_MAXHOST],service[NI_MAXSERV];
				getnameinfo(&tmp->addr.addr,tmp->addr.len,host,NI_MAXHOST,service,NI_MAXSERV,NI_NUMERICSERV|NI_NUMERICHOST);
				printf("disconnected : %s:%s\n",host,service);
			}
		}
	}
	server->to_remove=server->clients;
	struct msg_packet data;
	memset(data.msg,0,MAX_MSG_LEN);
	data.type=0;
	data.subtype=S_TYPE_CONNECT;
	sendtoall(server,&data);
}
void start_server(struct server_data * server,int port){
	server->fd=open_sock();
	local_bind(server->fd,port);
	pthread_mutex_init(&server->lock,0);
	server->clients=NULL;
	server->handlers=NULL;
	server->repeaters=NULL;
	server->to_remove=NULL;
	/*
	if(port){
		struct addr ret=get_client("127.0.0.1",port);
		struct msg_packet data;
		memset(data.msg,0,MAX_MSG_LEN);
		data.type=0;
		data.subtype=S_TYPE_PING;
		sendto(server->fd,&data,sizeof(struct msg_packet),0,&ret.addr,ret.len);
	}
	*/
	int status=1;
	setsockopt(server->fd,SOL_SOCKET,SO_BROADCAST,&status,sizeof(status));
	add_handler(server,main_handl_function,0,0);
	add_repeater(server,clean_old_connects);
	pthread_create(&server->server,0,server_main_loop,server);
	pthread_create(&server->repeat,0,repeat_main_loop,server);
	//pthread_create(&server->broadcast,0,broadcast,server);
}



void read_new_msg(struct server_data * server,const struct msg_packet * msg,const struct addr * addr){
	char host[NI_MAXHOST],service[NI_MAXSERV];
	getnameinfo(&addr->addr,addr->len,host,NI_MAXHOST,service,NI_MAXSERV,NI_NUMERICSERV|NI_NUMERICHOST);
	printf("%s:%s : %s",host,service,msg->msg);
}
void write_new_msg(struct server_data * server,const char msg[]){
	struct msg_packet data;
	data.type=1;
	data.subtype=1;
	strcpy(data.msg,msg);
	sendtoall(server,&data);
}
void cmd_connect(char *arg,struct server_data * s){
	char host[NI_MAXHOST];
	int port;
	sscanf(arg,"%s%d",host,&port);
	struct addr addr=get_client(host,port);
	connect_to_client(s,&addr);
}
void cmd_who(struct server_data * s){
	struct client_list * cur=s->clients;
	while(cur){
		if(cur->addr.len){
			char host[NI_MAXHOST],service[NI_MAXSERV];
			getnameinfo(&cur->addr.addr,cur->addr.len,host,NI_MAXHOST,service,NI_MAXSERV,NI_NUMERICSERV|NI_NUMERICHOST);
			printf("%s:%s\n",host,service);
		}
		cur=cur->next;
	}
}
void command(char *cmd_str,struct server_data * s){
	char cmd[64];
	sscanf(cmd_str,"%s",cmd);
	int l=strlen(cmd);
	if(!strcmp(cmd,"who"))cmd_who(s);
	if(!strcmp(cmd,"say"))write_new_msg(s,cmd_str+l+1);
	if(!strcmp(cmd,"connect"))cmd_connect(cmd_str+l+1,s);
}
int main(int argc, char *argv[]){
	//printf("starting server...\n");
	{
		char namebuf[512];
		if(DEBUG_MODE) 	sprintf(namebuf,"./log_%d_for_%d.log",time(NULL),getpid());
		else 			sprintf(namebuf,"/dev/null");
		log_fd=open(namebuf,O_RDWR|O_CREAT|O_TRUNC,0666);
	}
	struct server_data server;
	if(argc==2)start_server(&server,atoi(argv[1]));
	else start_server(&server,0);
	add_handler(&server,read_new_msg,1,1);
	//printf("server started\n");
	while(1){
		char buf[MAX_MSG_LEN];
		fgets(buf,MAX_MSG_LEN,stdin);
		buf[MAX_MSG_LEN-1]=0;
		if(buf[0]=='/')command(buf+1,&server);
		else {
			write_new_msg(&server,buf);
		}
	}
	return 0;
}
