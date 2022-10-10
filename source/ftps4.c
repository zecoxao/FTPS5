/*
 * Copyright (c) 2015 Sergi Granell (xerpi)
 * Copyright (c) 2016 Antonio Jose Ramos Marquez aka (bigboss) @psxdev
 */

#include "ftps4.h"

#define UNUSED(x) (void)(x)



void ftp_fini();


#define PATH_MAX 256
#define TemporaryNameSize 4096

#define NET_INIT_SIZE 1*1024*1024
#define FILE_BUF_SIZE 0x200

#define FTP_DEFAULT_PATH "/"

typedef enum {
	FTP_DATA_CONNECTION_NONE,
	FTP_DATA_CONNECTION_ACTIVE,
	FTP_DATA_CONNECTION_PASSIVE,
} DataConnectionType;

typedef struct ClientInfo {
	/* Client number */
	int num;
	/* Thread UID */
	ScePthread thid;
	/* Control connection socket FD */
	int ctrl_sockfd;
	/* Data connection attributes */
	int data_sockfd;
	DataConnectionType data_con_type;
	struct sockaddr_in data_sockaddr;
	/* PASV mode client socket */
	struct sockaddr_in pasv_sockaddr;
	int pasv_sockfd;
	/* Remote client net info */
	struct sockaddr_in addr;
	/* Receive buffer attributes */
	int n_recv;
	char recv_buffer[512];
	/* Current working directory */
	char cur_path[PATH_MAX];
	/* Rename path */
	char rename_path[PATH_MAX];
	/* Client list */
	struct ClientInfo *next;
	struct ClientInfo *prev;
} ClientInfo;

typedef void (*cmd_dispatch_func)(ClientInfo *client);

typedef struct {
	const char *cmd;
	cmd_dispatch_func func;
} cmd_dispatch_entry;

void *net_memory = NULL;
int ftp_initialized = 0;
struct in_addr ps4_addr;
unsigned short int ps4_port;
ScePthread server_thid;
int server_sockfd;
int number_clients = 0;
ClientInfo *client_list = NULL;
ScePthreadMutex client_list_mtx;


#define client_send_ctrl_msg(cl, str) \
	f_sceNetSend(cl->ctrl_sockfd, str, f_strlen(str), 0)

static inline void client_send_data_msg(ClientInfo *client, const char *str)
{
	if (client->data_con_type == FTP_DATA_CONNECTION_ACTIVE) {
		f_sceNetSend(client->data_sockfd, str, f_strlen(str), 0);
	} else {
		f_sceNetSend(client->pasv_sockfd, str, f_strlen(str), 0);
	}
}

static inline int client_send_recv_raw(ClientInfo *client, void *buf, unsigned int len)
{
	if (client->data_con_type == FTP_DATA_CONNECTION_ACTIVE) {
		return f_sceNetRecv(client->data_sockfd, buf, len, 0);
	} else {
		return f_sceNetRecv(client->pasv_sockfd, buf, len, 0);
	}
}

static inline void client_send_data_raw(ClientInfo *client, const void *buf, unsigned int len)
{
	if (client->data_con_type == FTP_DATA_CONNECTION_ACTIVE) {
		f_sceNetSend(client->data_sockfd, buf, len, 0);
	} else {
		f_sceNetSend(client->pasv_sockfd, buf, len, 0);
	}
}

static void cmd_USER_func(ClientInfo *client)
{
	client_send_ctrl_msg(client, "331 Username OK, need password b0ss.\n");
}

static void cmd_PASS_func(ClientInfo *client)
{
	client_send_ctrl_msg(client, "230 User logged in!\n");
}

static void cmd_QUIT_func(ClientInfo *client)
{
	client_send_ctrl_msg(client, "221 Goodbye senpai :'(\n");
	
}

static void cmd_SYST_func(ClientInfo *client)
{
	client_send_ctrl_msg(client, "215 UNIX Type: L8\n");
}

static void cmd_PASV_func(ClientInfo *client)
{
	int ret;
	UNUSED(ret);

	char cmd[512];
	unsigned int namelen;
	struct sockaddr_in picked;

	/* Create data mode socket name */
	char data_socket_name[64];
	f_sprintf(data_socket_name, "FTPS4_client_%i_data_socket",client->num);

	/* Create the data socket */
	client->data_sockfd = f_sceNetSocket(data_socket_name,
		AF_INET,
		SOCK_STREAM,
		0);

	//DEBUG("[PS4FTP] PASV data socket fd: %d\n", client->data_sockfd);

	/* Fill the data socket address */
	client->data_sockaddr.sin_len = sizeof(client->data_sockaddr);
	client->data_sockaddr.sin_family = AF_INET;
	client->data_sockaddr.sin_addr.s_addr = f_sceNetHtonl(INADDR_ANY);
	/* Let the PS4 choose a port */
	client->data_sockaddr.sin_port = f_sceNetHtons(0);

	/* Bind the data socket address to the data socket */
	ret = f_sceNetBind(client->data_sockfd,
		(struct sockaddr *)&client->data_sockaddr,
		sizeof(client->data_sockaddr));
	//DEBUG("[PS4FTP] f_sceNetBind(): 0x%08X\n", ret);

	/* Start listening */
	ret = f_sceNetListen(client->data_sockfd, 128);
	//DEBUG("[PS4FTP] f_sceNetListen(): 0x%08X\n", ret);

	/* Get the port that the PS4 has chosen */
	namelen = sizeof(picked);
	f_sceNetGetsockname(client->data_sockfd, (struct sockaddr *)&picked,
		&namelen);

	//DEBUG("[PS4FTP] PASV mode port: 0x%04X\n", picked.sin_port);

	/* Build the command */
	f_sprintf(cmd, "227 Entering Passive Mode (%hhu,%hhu,%hhu,%hhu,%hhu,%hhu)\n",
		(ps4_addr.s_addr >> 0) & 0xFF,
		(ps4_addr.s_addr >> 8) & 0xFF,
		(ps4_addr.s_addr >> 16) & 0xFF,
		(ps4_addr.s_addr >> 24) & 0xFF,
		(picked.sin_port >> 0) & 0xFF,
		(picked.sin_port >> 8) & 0xFF);

	client_send_ctrl_msg(client, cmd);

	/* Set the data connection type to passive! */
	client->data_con_type = FTP_DATA_CONNECTION_PASSIVE;
}

static void cmd_PORT_func(ClientInfo *client)
{
	unsigned char data_ip[4];
	unsigned char porthi, portlo;
	unsigned short data_port;
	char ip_str[16];
	struct in_addr data_addr;

	f_sscanf(client->recv_buffer, "%*s %hhu,%hhu,%hhu,%hhu,%hhu,%hhu",
		&data_ip[0], &data_ip[1], &data_ip[2], &data_ip[3],
		&porthi, &portlo);

	data_port = portlo + porthi*256;

	/* Convert to an X.X.X.X IP string */
	f_sprintf(ip_str, "%d.%d.%d.%d",
		data_ip[0], data_ip[1], data_ip[2], data_ip[3]);

	/* Convert the IP to a struct in_addr */
	f_sceNetInetPton(AF_INET, ip_str, &data_addr);

	//DEBUG("[PS4FTP] PORT connection to client's IP: %s Port: %d\n", ip_str, data_port);

	/* Create data mode socket name */
	char data_socket_name[64];
	f_sprintf(data_socket_name, "FTPS4_client_%i_data_socket",
		client->num);

	/* Create data mode socket */
	client->data_sockfd = f_sceNetSocket(data_socket_name,
		AF_INET,
		SOCK_STREAM,
		0);

	//DEBUG("[PS4FTP] Client %i data socket fd: %d\n", client->num,client->data_sockfd);

	/* Prepare socket address for the data connection */
	client->data_sockaddr.sin_len = sizeof(client->data_sockaddr);
	client->data_sockaddr.sin_family = AF_INET;
	client->data_sockaddr.sin_addr = data_addr;
	client->data_sockaddr.sin_port = f_sceNetHtons(data_port);

	/* Set the data connection type to active! */
	client->data_con_type = FTP_DATA_CONNECTION_ACTIVE;

	client_send_ctrl_msg(client, "200 PORT command successful!\n");
}

static void client_open_data_connection(ClientInfo *client)
{
	int ret;
	UNUSED(ret);

	unsigned int addrlen;

	if (client->data_con_type == FTP_DATA_CONNECTION_ACTIVE) {
		/* Connect to the client using the data socket */
		ret = f_sceNetConnect(client->data_sockfd,
			(struct sockaddr *)&client->data_sockaddr,
			sizeof(client->data_sockaddr));

		//DEBUG("[PS4FTP] f_sceNetConnect(): 0x%08X\n", ret);
	} else {
		/* Listen to the client using the data socket */
		addrlen = sizeof(client->pasv_sockaddr);
		client->pasv_sockfd = f_sceNetAccept(client->data_sockfd,
			(struct sockaddr *)&client->pasv_sockaddr,
			&addrlen);
		//DEBUG("[PS4FTP] PASV client fd: 0x%08X\n", client->pasv_sockfd);
	}
}

static void client_close_data_connection(ClientInfo *client)
{
	f_sceNetSocketClose(client->data_sockfd);
	/* In passive mode we have to close the client pasv socket too */
	if (client->data_con_type == FTP_DATA_CONNECTION_PASSIVE) {
		f_sceNetSocketClose(client->pasv_sockfd);
	}
	client->data_con_type = FTP_DATA_CONNECTION_NONE;
}

static char file_type_char(mode_t mode)
{
	return S_ISBLK(mode) ? 'b' :
		S_ISCHR(mode) ? 'c' :
		S_ISREG(mode) ? '-' :
		S_ISDIR(mode) ? 'd' :
		S_ISFIFO(mode) ? 'p' :
		S_ISSOCK(mode) ? 's' :
		S_ISLNK(mode) ? 'l' : ' ';
}

static int gen_list_format(char *out, int n, mode_t mode, unsigned int file_size,
	int month_n, int day_n, int hour, int minute, const char *filename)
{
	static const char num_to_month[][4] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};

	return f_snprintf(out, n,
		"%c%s 1 ps4 ps4 %d %s %-2d %02d:%02d %s\r\n",
		file_type_char(mode),
		S_ISDIR(mode) ? "rwxr-xr-x" : "rw-r--r--",
		file_size,
		num_to_month[month_n%12],
		day_n,
		hour,
		minute,
		filename);
}

static void send_LIST(ClientInfo *client, const char *path)
{
	char buffer[512];
	char *dentbuf;
	struct dirent *dent;
	int dfd;
	struct stat st;
	struct tm * tm;
	int i;
	char *temporaryName;
	
	temporaryName = f_malloc(TemporaryNameSize);
	if(temporaryName == NULL)
	{
		//DEBUG("error calling f_malloc\n");
		return;
	}

	dfd = f_open(path, O_RDONLY, 0);
	if (dfd < 0) {
		client_send_ctrl_msg(client, "550 Invalid directory.\n");
		return;
	}

	//memset(dentbuf, 0, sizeof(dentbuf));

	client_send_ctrl_msg(client, "150 Opening ASCII mode data transfer for LIST.\n");
	client_open_data_connection(client);


	int err=f_fstat(dfd, &st);
	if(err<0)
	{
		//DEBUG( "fstat error return  0x%08X \n",err);
		return;
	}
	dentbuf=f_mmap(NULL, st.st_blksize+sizeof(struct dirent), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (dentbuf)
	{
		// Make sure we will have a null terminated entry at the end.Thanks people leaving CryEngine code for orbis on github  :)
		for(i=0;i<st.st_blksize+sizeof(struct dirent);i++)
		{
			dentbuf[i]=0;
		}
		err=f_getdents(dfd, dentbuf, st.st_blksize);

		int nOffset = err;
		while (err > 0 && err < st.st_blksize)
		{
			err = f_getdents(dfd, dentbuf + nOffset, st.st_blksize-nOffset);
			nOffset += err;
		}
		
		if (err>0)
			err=0;
		
		
		
		
		
		
		
		dent = (struct dirent *)dentbuf;
		while(dent->d_fileno ) {
			
			f_strcpy(temporaryName, path);
			size_t l = f_strlen(path);
			if(l > 0 && path[l - 1] != '/')
			{
				f_strcat(temporaryName, "/");
			}
			f_strcat(temporaryName, dent->d_name);
			
			err=f_stat(temporaryName, &st);
			if ( err== 0) 
			{
				
				 tm = f_localtime(&(st.st_ctim));
				 
				 gen_list_format(buffer, sizeof(buffer),
					st.st_mode,
					st.st_size,
					tm->tm_mon,
					tm->tm_mday,
					tm->tm_hour,
					tm->tm_min,
					dent->d_name);
				
				client_send_data_msg(client, buffer);
			}
			else
			{
				//DEBUG("[PS4FTP] error stat %d %s\n",err,dent->d_name);
			}
			
			dent = (struct dirent *)((void *)dent + dent->d_reclen);
				
			f_memset(buffer, 0, sizeof(buffer));
				
		}
	}
	f_munmap(dentbuf,st.st_blksize+sizeof(struct dirent));
	f_free(temporaryName);


	

	f_close(dfd);

	//DEBUG("[PS4FTP] Done sending LIST\n");

	client_close_data_connection(client);
	client_send_ctrl_msg(client, "226 Transfer complete.\n");
}


static void cmd_LIST_func(ClientInfo *client)
{
	char list_path[PATH_MAX];

	int n = f_sscanf(client->recv_buffer, "%*s %[^\r\n\t]", list_path);

	if (n > 0) {  /* Client specified a path */
		send_LIST(client, list_path);
	} else {      /* Use current path */
		send_LIST(client, client->cur_path);
	}
}

static void cmd_PWD_func(ClientInfo *client)
{
	char msg[PATH_MAX];
	f_sprintf(msg, "257 \"%s\" is the current directory.\n", client->cur_path);
	client_send_ctrl_msg(client, msg);
}

static void cmd_CWD_func(ClientInfo *client)
{
	char cmd_path[PATH_MAX];
	char path[PATH_MAX];
	int pd;
	int n = f_sscanf(client->recv_buffer, "%*s %[^\r\n\t]", cmd_path);

	if (n < 1) {
		client_send_ctrl_msg(client, "500 Syntax error, command unrecognized.\n");
	} else {
		if (cmd_path[0] != '/') { /* Change dir relative to current dir */
			f_sprintf(path, "%s%s", client->cur_path, cmd_path);
		} else {
			f_strcpy(path, cmd_path);
		}

		/* If there isn't "/" at the end, add it */
		if (path[f_strlen(path) - 1] != '/') {
			f_strcat(path, "/");
		}

		/* Check if the path exists */
		pd = f_open(path, O_RDONLY, 0);
		if (pd < 0) {
			client_send_ctrl_msg(client, "550 Invalid directory.\n");
			return;
		}
		f_close(pd);

		f_strcpy(client->cur_path, path);
		client_send_ctrl_msg(client, "250 Requested file action okay, completed.\n");
	}
}

static void cmd_TYPE_func(ClientInfo *client)
{
	char data_type;
	char format_control[8];
	int n_args = f_sscanf(client->recv_buffer, "%*s %c %s", &data_type, format_control);

	if (n_args > 0) {
		switch(data_type) {
		case 'A':
		case 'I':
			client_send_ctrl_msg(client, "200 Okay\n");
			break;
		case 'E':
		case 'L':
		default:
			client_send_ctrl_msg(client, "504 Error: bad parameters?\n");
			break;
		}
	} else {
		client_send_ctrl_msg(client, "504 Error: bad parameters?\n");
	}
}

static void dir_up(char *path)
{
	char *pch;
	size_t len_in = f_strlen(path);
	if (len_in == 1) {
		f_strcpy(path, "/");
		return;
	}
	if (path[len_in - 1] == '/') {
		path[len_in - 1] = '\0';
	}
	pch = f_strrchr(path, '/');
	if (pch) {
		size_t s = len_in - (pch - path);
		f_memset(pch + 1, '\0', s);
	}
}

static void cmd_CDUP_func(ClientInfo *client)
{
	dir_up(client->cur_path);
	client_send_ctrl_msg(client, "200 Command okay.\n");
}

static void send_file(ClientInfo *client, const char *path)
{
	unsigned char *buffer;
	int fd;
	unsigned int bytes_read;

	//DEBUG("[PS4FTP] Opening: %s\n", path);

	if ((fd = f_open(path, O_RDONLY, 0)) >= 0) {

		buffer = f_malloc(FILE_BUF_SIZE);
		if (buffer == NULL) {
			client_send_ctrl_msg(client, "550 Could not allocate memory.\n");
			return;
		}

		client_open_data_connection(client);
		client_send_ctrl_msg(client, "150 Opening Image mode data transfer.\n");

		while ((bytes_read = f_read(fd, buffer, FILE_BUF_SIZE)) > 0) {
			client_send_data_raw(client, buffer, bytes_read);
		}

		f_close(fd);
		f_free(buffer);
		client_send_ctrl_msg(client, "226 Transfer completed.\n");
		client_close_data_connection(client);

	} else {
		client_send_ctrl_msg(client, "550 File not found.\n");
	}
}

/* This function generates a PS4 valid path with the input path
 * from RETR, STOR, DELE, RMD, MKD, RNFR and RNTO commands */
static void gen_filepath(ClientInfo *client, char *dest_path)
{
	char cmd_path[PATH_MAX];
	f_sscanf(client->recv_buffer, "%*[^ ] %[^\r\n\t]", cmd_path);

	if (cmd_path[0] != '/') { /* The file is relative to current dir */
		/* Append the file to the current path */
		f_sprintf(dest_path, "%s%s", client->cur_path, cmd_path);
	} else {
		f_strcpy(dest_path, cmd_path);
	}
}

static void cmd_RETR_func(ClientInfo *client)
{
	char dest_path[PATH_MAX];
	gen_filepath(client, dest_path);
	send_file(client, dest_path);
}

static void receive_file(ClientInfo *client, const char *path)
{
	unsigned char *buffer;
	int fd;
	unsigned int bytes_recv;

	//DEBUG("[PS4FTP] Opening: %s\n", path);

	if ((fd = f_open(path, O_CREAT | O_WRONLY | O_TRUNC, 0777)) >= 0) {

		buffer = f_malloc(FILE_BUF_SIZE);
		if (buffer == NULL) {
			client_send_ctrl_msg(client, "550 Could not allocate memory.\n");
			return;
		}

		client_open_data_connection(client);
		client_send_ctrl_msg(client, "150 Opening Image mode data transfer.\n");

		while ((bytes_recv = client_send_recv_raw(client, buffer, FILE_BUF_SIZE)) > 0) {
			f_write(fd, buffer, bytes_recv);
		}

		f_close(fd);
		f_free(buffer);
		client_send_ctrl_msg(client, "226 Transfer completed.\n");
		client_close_data_connection(client);

	} else {
		client_send_ctrl_msg(client, "550 File not found.\n");
	}
}

static void cmd_STOR_func(ClientInfo *client)
{
	char dest_path[PATH_MAX];
	gen_filepath(client, dest_path);
	receive_file(client, dest_path);
}

static void delete_file(ClientInfo *client, const char *path)
{
	//DEBUG("[PS4FTP] Deleting: %s\n", path);

	if (f_unlink(path) >= 0) {
		client_send_ctrl_msg(client, "226 File deleted.\n");
	} else {
		client_send_ctrl_msg(client, "550 Could not delete the file.\n");
	}
}

static void cmd_DELE_func(ClientInfo *client)
{
	char dest_path[PATH_MAX];
	gen_filepath(client, dest_path);
	delete_file(client, dest_path);
}

static void delete_dir(ClientInfo *client, const char *path)
{
	int ret;
	//DEBUG("[PS4FTP] Deleting: %s\n", path);
	ret = f_rmdir(path);
	if (ret >= 0) {
		client_send_ctrl_msg(client, "226 Directory deleted.\n");
	} else if (ret == 0x8001005A) { /* DIRECTORY_IS_NOT_EMPTY */
		client_send_ctrl_msg(client, "550 Directory is not empty.\n");
	} else {
		client_send_ctrl_msg(client, "550 Could not delete the directory.\n");
	}
}

static void cmd_RMD_func(ClientInfo *client)
{
	char dest_path[PATH_MAX];
	gen_filepath(client, dest_path);
	delete_dir(client, dest_path);
}

static void create_dir(ClientInfo *client, const char *path)
{
	//DEBUG("[PS4FTP] Creating: %s\n", path);

	if (f_mkdir(path, 0777) >= 0) {
		client_send_ctrl_msg(client, "226 Directory created.\n");
	} else {
		client_send_ctrl_msg(client, "550 Could not create the directory.\n");
	}
}

static void cmd_MKD_func(ClientInfo *client)
{
	char dest_path[PATH_MAX];
	gen_filepath(client, dest_path);
	create_dir(client, dest_path);
}

static int file_exists(const char *path)
{
	struct stat s;
	return (f_stat(path, &s) >= 0);
}

static void cmd_RNFR_func(ClientInfo *client)
{
	char from_path[PATH_MAX];
	/* Get the origin filename */
	gen_filepath(client, from_path);

	/* Check if the file exists */
	if (!file_exists(from_path)) {
		client_send_ctrl_msg(client, "550 The file doesn't exist.\n");
		return;
	}
	/* The file to be renamed is the received path */
	f_strcpy(client->rename_path, from_path);
	client_send_ctrl_msg(client, "250 I need the destination name b0ss.\n");
}

static void cmd_RNTO_func(ClientInfo *client)
{
	char path_to[PATH_MAX];
	/* Get the destination filename */
	gen_filepath(client, path_to);

	//DEBUG("[PS4FTP] Renaming: %s to %s\n", client->rename_path, path_to);

	if (f_rename(client->rename_path, path_to) < 0) {
		client_send_ctrl_msg(client, "550 Error renaming the file.\n");
	}

	client_send_ctrl_msg(client, "226 Rename completed.\n");
}

static void cmd_SIZE_func(ClientInfo *client)
{
	struct stat s;
	char path[PATH_MAX];
	char cmd[64];
	/* Get the filename to retrieve its size */
	gen_filepath(client, path);

	/* Check if the file exists */
	if (f_stat(path, &s) < 0) {
		client_send_ctrl_msg(client, "550 The file doesn't exist.\n");
		return;
	}
	/* Send the size of the file */
	f_sprintf(cmd, "213: %lld\n", s.st_size);
	client_send_ctrl_msg(client, cmd);
}

#define add_entry(name) {#name, cmd_##name##_func}
static const cmd_dispatch_entry cmd_dispatch_table[] = {
	add_entry(USER),
	add_entry(PASS),
	add_entry(QUIT),
	add_entry(SYST),
	add_entry(PASV),
	add_entry(PORT),
	add_entry(LIST),
	add_entry(PWD),
	add_entry(CWD),
	add_entry(TYPE),
	add_entry(CDUP),
	add_entry(RETR),
	add_entry(STOR),
	add_entry(DELE),
	add_entry(RMD),
	add_entry(MKD),
	add_entry(RNFR),
	add_entry(RNTO),
	add_entry(SIZE),
	{NULL, NULL}
};

static cmd_dispatch_func get_dispatch_func(const char *cmd)
{
	//DEBUG("[+] cmd_dispatch_func: %s\n", cmd);

	if (!f_strcmp(cmd, "USER")) {
		return cmd_USER_func;
	} else if (!f_strcmp(cmd, "PASS")) {
		return cmd_PASS_func;
	} else if (!f_strcmp(cmd, "QUIT")) {
		return cmd_QUIT_func;
	} else if (!f_strcmp(cmd, "SYST")) {
		return cmd_SYST_func;
	} else if (!f_strcmp(cmd, "PASV")) {
		return cmd_PASV_func;
	} else if (!f_strcmp(cmd, "PORT")) {
		return cmd_PORT_func;
	} else if (!f_strcmp(cmd, "LIST")) {
		return cmd_LIST_func;
	} else if (!f_strcmp(cmd, "PWD")) {
		return cmd_PWD_func;
	} else if (!f_strcmp(cmd, "CWD")) {
		return cmd_CWD_func;
	} else if (!f_strcmp(cmd, "TYPE")) {
		return cmd_TYPE_func;
	} else if (!f_strcmp(cmd, "CDUP")) {
		return cmd_CDUP_func;
	} else if (!f_strcmp(cmd, "RETR")) {
		return cmd_RETR_func;
	} else if (!f_strcmp(cmd, "STOR")) {
		return cmd_STOR_func;
	} else if (!f_strcmp(cmd, "DELE")) {
		return cmd_DELE_func;
	} else if (!f_strcmp(cmd, "RMD")) {
		return cmd_RMD_func;
	} else if (!f_strcmp(cmd, "MKD")) {
		return cmd_MKD_func;
	} else if (!f_strcmp(cmd, "RNFR")) {
		return cmd_RNFR_func;
	} else if (!f_strcmp(cmd, "RNTO")) {
		return cmd_RNTO_func;
	} else if (!f_strcmp(cmd, "SIZE")) {
		return cmd_SIZE_func;
	}

	return NULL;
}

static void client_list_add(ClientInfo *client)
{
	/* Add the client at the front of the client list */
	f_scePthreadMutexLock(&client_list_mtx);

	if (client_list == NULL) { /* List is empty */
		client_list = client;
		client->prev = NULL;
		client->next = NULL;
	} else {
		client->next = client_list;
		client->next->prev = client;
		client->prev = NULL;
		client_list = client;
	}

	f_scePthreadMutexUnlock(&client_list_mtx);
}

static void client_list_delete(ClientInfo *client)
{
	/* Remove the client from the client list */
	f_scePthreadMutexLock(&client_list_mtx);

	if (client->prev) {
		client->prev->next = client->next;
	}
	if (client->next) {
		client->next->prev = client->prev;
	}

	f_scePthreadMutexUnlock(&client_list_mtx);
}

static void client_list_close_sockets()
{
	/* Iterate over the client list and close their sockets */
	f_scePthreadMutexLock(&client_list_mtx);

	ClientInfo *it = client_list;

	while (it) {
		f_sceNetSocketClose(it->ctrl_sockfd);
		it = it->next;
	}

	f_scePthreadMutexUnlock(&client_list_mtx);
}

static void *client_thread(void *arg)
{
	char cmd[16];
	cmd_dispatch_func dispatch_func;
	ClientInfo *client = (ClientInfo *)arg;

	//DEBUG("[PS4FTP] Client thread %i started!\n", client->num);

	client_send_ctrl_msg(client, "220 FTPS4 Server ready.\n");

	while (1) {

		f_memset(client->recv_buffer, 0, sizeof(client->recv_buffer));

		client->n_recv = f_sceNetRecv(client->ctrl_sockfd, client->recv_buffer, sizeof(client->recv_buffer), 0);
		if (client->n_recv > 0) {
			//DEBUG("[PS4FTP] Received %i bytes from client number %i:\n",client->n_recv, client->num);

			//INFO("\t%i> %s", client->num, client->recv_buffer);

			/* The command are the first chars until the first space */
			f_sscanf(client->recv_buffer, "%s", cmd);

			/* Wait 1 ms before sending any data */
			f_sceKernelUsleep(1*1000);

			if ((dispatch_func = get_dispatch_func(cmd))) {
				dispatch_func(client);
			} else {
				client_send_ctrl_msg(client, "502 Sorry, command not implemented. :(\n");
			}

		} else if (client->n_recv == 0) {
			/* Value 0 means connection closed by the remote peer */
			//INFO("Connection closed by the client %i.\n", client->num);
			/* Close the client's socket */
			f_sceNetSocketClose(client->ctrl_sockfd);
			/* Delete itself from the client list */
			client_list_delete(client);
			break;
		} else {
			/* A negative value means error */
			break;
		}
	}

	/* If there's an open data connection, close it */
	if (client->data_con_type != FTP_DATA_CONNECTION_NONE) {
		f_sceNetSocketClose(client->data_sockfd);
		if (client->data_con_type == FTP_DATA_CONNECTION_PASSIVE) {
			f_sceNetSocketClose(client->pasv_sockfd);
		}
	}

	//DEBUG("[PS4FTP] Client thread %i exiting!\n", client->num);

	f_free(client);

	//f_scePthreadExit(NULL);
	return NULL;
}

static void *server_thread(void *arg)
{
	int ret;
	UNUSED(ret);

	struct sockaddr_in serveraddr;

	//DEBUG("[PS4FTP] Server thread started!\n");

	/* Create server socket */
	server_sockfd = f_sceNetSocket("FTPS4_server_sock",
		AF_INET,
		SOCK_STREAM,
		0);

	//DEBUG("[PS4FTP] Server socket fd: %d\n", server_sockfd);

	/* Fill the server's address */
	serveraddr.sin_len = sizeof(serveraddr);
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_addr.s_addr = f_sceNetHtonl(INADDR_ANY);
	serveraddr.sin_port = f_sceNetHtons(ps4_port);

	/* Bind the server's address to the socket */
	ret = f_sceNetBind(server_sockfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr));
	//DEBUG("[PS4FTP] f_sceNetBind(): 0x%08X\n", ret);

	/* Start listening */
	ret = f_sceNetListen(server_sockfd, 128);
	//DEBUG("[PS4FTP] f_sceNetListen(): 0x%08X\n", ret);

	while (1) {

		/* Accept clients */
		struct sockaddr_in clientaddr;
		int client_sockfd;
		unsigned int addrlen = sizeof(clientaddr);

		//DEBUG("[PS4FTP] Waiting for incoming connections on port: %d...\n", ps4_port);

		client_sockfd = f_sceNetAccept(server_sockfd, (struct sockaddr *)&clientaddr, &addrlen);
		if (client_sockfd >= 0) {

			//DEBUG("[PS4FTP] New connection, client fd: 0x%08X\n", client_sockfd);

			/* Get the client's IP address */
			char remote_ip[16];
			f_sceNetInetNtop(AF_INET,
				&clientaddr.sin_addr.s_addr,
				remote_ip,
				sizeof(remote_ip));

			//INFO("Client %i connected, IP: %s port: %i\n",number_clients, remote_ip, clientaddr.sin_port);

			/* Allocate the ClientInfo struct for the new client */
			ClientInfo *client = f_malloc(sizeof(*client));
			client->num = number_clients;
			client->ctrl_sockfd = client_sockfd;
			client->data_con_type = FTP_DATA_CONNECTION_NONE;
			f_strcpy(client->cur_path, FTP_DEFAULT_PATH);
            f_memcpy(&client->addr, &clientaddr, sizeof(client->addr));

			/* Add the new client to the client list */
			client_list_add(client);

			/* Create a new thread for the client */
			char client_thread_name[64];
			f_sprintf(client_thread_name, "FTPS4_client_%i_thread",
				number_clients);

			/* Create a new thread for the client */
			f_scePthreadCreate(&client->thid, NULL, client_thread, client, client_thread_name);

			//DEBUG("[PS4FTP] Client %i thread UID: 0x%08X\n", number_clients, client->thid);

			number_clients++;
		} else {
			/* if f_sceNetAccept returns < 0, it means that the listening
			 * socket has been closed, this means that we want to
			 * finish the server thread */
			//DEBUG("[PS4FTP] Server socket closed, 0x%08X\n", client_sockfd);
			break;
		}
	}

	//DEBUG("[PS4FTP] Server thread exiting!\n");
	
	return NULL;
}

void ftp_init(const char *ip, unsigned short int port)
{
	int ret;
	UNUSED(ret);

	//SceNetInitParam initparam;
	//SceNetCtlInfo info;

	if (ftp_initialized) {
		return;
	}

	/* Init Net */
	/*if (f_sceNetShowNetf_stat() == PSP2_NET_ERROR_ENOTINIT) {
		net_memory = f_malloc(NET_INIT_SIZE);
		initparam.memory = net_memory;
		initparam.size = NET_INIT_SIZE;
		initparam.flags = 0;
		ret = f_sceNetInit(&initparam);
		//DEBUG("f_sceNetInit(): 0x%08X\n", ret);
	} else {
		//DEBUG("Net is already initialized.\n");
	}*/

	/* Init NetCtl */
	//ret = f_sceNetCtlInit();
	////DEBUG("f_sceNetCtlInit(): 0x%08X\n", ret);

	/* Get IP address */
	//ret = f_sceNetCtlInetGetInfo(PSP2_NETCTL_//INFO_GET_IP_ADDRESS, &info);
	////DEBUG("f_sceNetCtlInetGetInfo(): 0x%08X\n", ret);

	/* Return data */
	//f_strcpy(vita_ip, info.ip_address);
	//*vita_port = FTP_PORT;

	/* Save the listening port of the PS4 to a global variable */
	ps4_port = port;

	/* Save the IP of the PS4 to a global variable */
	f_sceNetInetPton(AF_INET, ip, &ps4_addr);

	/* Create the client list mutex */
	f_scePthreadMutexInit(&client_list_mtx, NULL, "FTPS4_client_list_mutex");
	//DEBUG("[PS4FTP] Client list mutex UID: 0x%08X\n", client_list_mtx);

	/* Create server thread */
	f_scePthreadCreate(&server_thid, NULL, server_thread, NULL, "FTPS4_server_thread");
	//DEBUG("[PS4FTP] Server thread UID: 0x%08X\n", server_thid);

	ftp_initialized = 1;
	
	f_scePthreadJoin(server_thid, NULL);
	
	
}

void ftp_fini()
{
	if (ftp_initialized) {
		/* In order to "stop" the blocking f_sceNetAccept,
		 * we have to close the server socket; this way
		 * the accept call will return an error */
		f_sceNetSocketClose(server_sockfd);
		//f_sceNetSocketAbort(server_sockfd,1);
		/* To close the clients we have to do the same:
		 * we have to iterate over all the clients
		 * and close their sockets */
		client_list_close_sockets();
		client_list = NULL;

		/* UGLY: Give 50 ms for the threads to exit */
		f_sceKernelUsleep(50*1000);

		/* Delete the client list mutex */
		f_scePthreadMutexDestroy(client_list_mtx);

		//f_sceNetCtlTerm();
		//f_sceNetTerm();

		

		ftp_initialized = 0;
	}
}