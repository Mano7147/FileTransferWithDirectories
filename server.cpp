#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h> //FD_SET, FD_ISSET, FD_ZERO macros
#include <arpa/inet.h>    //close
#include <netinet/in.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
 
#define BUFFER_SIZE (1024)
#define MAX_PATH_SIZE (1024)
#define MD5_SIZE (32)
#define PORT 4444
#define SIZE_INT (4)
#define SIZE_MD5 (32)
#define MAX_NAME_SIZE 1000000
 
using namespace std;
 
typedef long long ll;

enum StateFile { FILE_NAME, FILE_CHECK, FILE_DATA};
enum StateDir { DIR_NAME, DIR_MAKE};
enum State {RECV_TYPE, RECV_FILE, RECV_DIR, RECV_STOP, RECV_ERR_STOP};

void change_to_safe_name(char* name, int size_name) {
    int i;
    for (i = 0; i < size_name; ++i) {
        if (i + 2 < size_name && name[i] == '.' && name[i + 1] == '.' && name[i + 2] == '/') {
            name[i] = 'd';
            name[i + 1] = 'd';
            name[i + 2] = 'd';
        }
        if (i + 1 < size_name && name[i] == '.' && name[i + 1] == '/') {
            name[i] = 'd';
            name[i + 1] = 'd';
        }
    }
}
 
struct Connection {
    int csocket;  

    void* buf;
    int pbuf;
    int all_recv;
    int szbuf;
    char* cwd;
    
    char* name;
    char* file_name;
    int pname;  
    int size_name; 
    bool flag_recv_size_name;   
    
    int fd;
    int cur_size_fd;
    int file_size;
    bool flag_recv_size_file;

    unsigned char md5buf[SIZE_MD5 + 1];

    State state;
    StateFile stateFile;
    StateDir stateDir;

    bool buffer_empty() {
        return pbuf == 0;
    }

    void init_recv() {
        flag_recv_size_name = false;
        flag_recv_size_file = false;
        fd = 0;
        cur_size_fd = 0;
    }

    void do_open_cwd() {
        int size = pathconf(".", _PC_PATH_MAX) + MAX_PATH_SIZE;
        cwd = (char*)malloc(size);
        getcwd(cwd, size);
        strcat(cwd, "/source_dir");

        int res = mkdir(cwd, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
        if (res != 0) {
            perror("do_open_cwd");
            return;
        }
    }

    int open_temp_file(int i) {
        int size = pathconf(".", _PC_PATH_MAX);
        cwd = (char*)malloc(size);
        getcwd(cwd, size);
        char full_filename[MAX_PATH_SIZE];
        char tmp[MAX_PATH_SIZE];
        sprintf(tmp, "tmp%d", i);
        getcwd(cwd, sizeof(cwd));
        strcat(strcat(strcpy(full_filename, cwd), "/source_dir/"), tmp);
        return open(full_filename, O_WRONLY | O_CREAT, S_IRWXU);
    }

    void move_buf(int size) {
        if (size > pbuf) {
            fprintf(stderr, "Bad size mave buf\n");
            return;
        }
        memmove(buf, (char*)buf + size, pbuf - size);
        pbuf -= size;
    }
 
    Connection(int csocket) {
        init_recv();
        buf = malloc(BUFFER_SIZE + 1);
        pbuf = 0;
        szbuf = 0;
        all_recv = 0;
        fd = 0; 
        cur_size_fd = 0;
        this->csocket = csocket;

        do_open_cwd();
        state = RECV_TYPE;        
    }

    int recv_name() {
        fprintf(stderr, "%s\n", "Receive name.");
        
        if (!flag_recv_size_name) {
            if (pbuf < SIZE_INT)
                return 0;

            size_name = (int)ntohl(((uint32_t*)buf)[0]);
            move_buf(SIZE_INT);

            if (size_name <= 0 || size_name > MAX_NAME_SIZE) {
                fprintf(stderr, "Bad size of name: %d\n", size_name);
                return -1;
            }

            name = (char*)malloc(size_name);
            name[size_name - 1] = '\0';
            pname = 0;
            flag_recv_size_name = true;
        }
        else {
            if (pname < size_name) {
                int need = size_name - pname;
                int can = min(need, pbuf);
                memcpy(name + pname, buf, can);
                move_buf(can);
                pname += can;
            }
            else {
                fprintf(stderr, "Bad state in recv_name\n");
                return -1;
            }
            if (pname == size_name) {
                fprintf(stderr, "Name: %s\n", name);
                return 1;
            }
        }

        return 0;
    }

    int recv_directory() {
        char* dir_name;
        change_to_safe_name(name, size_name);
        dir_name = (char*)malloc(strlen(name) + strlen(cwd) + 2);
        strcat(strcat(strcpy(dir_name, cwd), "/"), name);
        
        int res = mkdir(dir_name, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
        fprintf(stderr, "Dir: %s\n", dir_name);
        if (res != 0) {
            perror("recv_directory");
            return -1;
        }

        free(name);
        free(dir_name);

        return 1;
    }

    int recv_file_check() {
        fprintf(stderr, "%s\n", "Receive file info.");

        if (!flag_recv_size_file) {
            if (pbuf < SIZE_INT)
                return 0;

            file_size = (int)ntohl(((uint32_t*)buf)[0]);
            if (file_size < 0) {
                fprintf(stderr, "%s: %d\n", "Bad file size", file_size);
                return -1;
            }
            fprintf(stderr, "File size: %d\n", file_size);
            move_buf(SIZE_INT); 
            flag_recv_size_file = true;           
        }
        else {
            if (pbuf < SIZE_MD5 + 1)
                return 0;
            
            memcpy(md5buf, buf, SIZE_MD5 + 1);
            md5buf[SIZE_MD5] = '\0';
            move_buf(SIZE_MD5 + 1);            
            fprintf(stderr, "%s\n", "Info has been received.");

            return 1;
        }

        return 0;
    }

    bool check_md5sum(char* file_name) {
        int i;
        int size;
        unsigned char md5buf1[SIZE_MD5 + 1];
        char* command = (char*)malloc(strlen(file_name) + 50);
        strcat(strcpy(command, "md5sum "), file_name);
        FILE* pipe = popen(command, "r");
        if (pipe == NULL) {
            perror("check_md5sum");
            return false;
        }
        size = fread((void*)md5buf1, sizeof(char), SIZE_MD5, pipe);
        if (size != SIZE_MD5) {
            perror("send_file_info md5");
            return false;
        }    
        fclose(pipe);
        free(command);
        md5buf1[SIZE_MD5] = '\0';

        for (i = 0; i < SIZE_MD5; ++i) {
            if (md5buf1[i] != md5buf[i])
                return false;
        }
        return true;
    }

    int recv_file_data() {             
        if (0 == fd) {            
            change_to_safe_name(name, size_name);
            file_name = (char*)malloc(strlen(name) + strlen(cwd) + 2);
            strcat(strcat(strcpy(file_name, cwd), "/"), name);
            fd = open(file_name, O_WRONLY | O_CREAT, S_IRWXU);
        }
        if (0 == fd) {
            perror("create file");
            free(name);
            free(file_name);
            return -1;
        }
        
        int need = min(pbuf, file_size - cur_size_fd);
        int size = write(fd, buf, need);
        if (size < 0) {
            perror("write to file");
            close(fd);   
            remove(file_name);
            free(name);
            free(file_name);
            return -1;
        }

        cur_size_fd += size;
        move_buf(size);
        if (cur_size_fd == file_size) {                     
            bool complete_file = check_md5sum(file_name);
            if (!complete_file) {
                fprintf(stderr, "%s\n", "MD5 test failed. File will be deleted.");
                close(fd);   
                remove(file_name);
                free(name);
                free(file_name);
                return -1;
            }
            fprintf(stderr, "Had been received: %s\n", name);            
            close(fd);
            free(name);
            free(file_name);
            return 1;
        } 

        return 0;
    }    

    int recv_type() {
        if (pbuf < 1)
            return 0;
        char type = ((char*)buf)[0];
        move_buf(1);
        switch (type) {
            case 'f' :
                fprintf(stderr, "%s\n", "Receive file:");
                return 1;
            case 'd' :
                fprintf(stderr, "%s\n", "Receive directory:");
                return 2;
            case 'x': 
                fprintf(stderr, "%s\n", "Received stop symbol.");
                return 3;
            default :
                fprintf(stderr, "Recv type not correct!\n");
                return -1;
        }
    }

    int do_what_state() {
        //fprintf(stderr, "%s\n", "Do what state");
        //printf("%d %d %d\n", (int)state, (int)stateFile, (int)stateDir);
        if (state == RECV_STOP) {
            return 1;
        }
        if (state == RECV_ERR_STOP) {
            return -1;
        }
        if (state == RECV_TYPE) {
            init_recv();
            int res = recv_type();
            if (1 == res) {
                state = RECV_FILE;
                stateFile = FILE_NAME;
            }
            else if (2 == res) {                
                state = RECV_DIR;
                stateDir = DIR_NAME;
            }
            else if (3 == res) {
                state = RECV_STOP;
            }
            else if (-1 == res) {
                state = RECV_ERR_STOP;
                return -1;
            }
        }
        else if (state == RECV_FILE) {
            if (stateFile == FILE_NAME){
                int res = recv_name();
                if (res == -1) {
                    state = RECV_ERR_STOP;
                    return -1;
                }
                if (res)
                    stateFile = FILE_CHECK;
            }
            else if (stateFile == FILE_CHECK) {
                int res = recv_file_check();
                if (res == -1) {
                    state = RECV_ERR_STOP;
                    return -1;
                }
                if (res) 
                    stateFile = FILE_DATA;                
            }                
            else if (stateFile == FILE_DATA) {
                int res = recv_file_data();
                if (res == -1) {
                    state = RECV_ERR_STOP;
                    return -1;
                }
                if (res) 
                    state = RECV_TYPE;                                                        
            }                
            else
                fprintf(stderr, "Error state\n");
        }
        else if (state == RECV_DIR) {
            if (stateDir == DIR_NAME){
                int res = recv_name();
                if (res == -1) {
                    state = RECV_STOP;
                    return -1;
                }
                if (res)
                    stateDir = DIR_MAKE;
            }
            else if (stateDir == DIR_MAKE) {
                int res = recv_directory();
                if (res == -1) {
                    state = RECV_ERR_STOP;
                    return -1;
                }
                if (res) 
                    state = RECV_TYPE;
            }                
            else
                fprintf(stderr, "Error state\n");
        }
        return 0;
        //printf("%d %d %d\n", (int)state, (int)stateFile, (int)stateDir);
    }

    int recv_to_buf() {
        if (pbuf == BUFFER_SIZE) // kostyl 
            return 1;

        int size = recv(csocket, (char*)buf + pbuf, BUFFER_SIZE - pbuf, 0);        
        if (size <= 0) {
            perror("recv_to_buf");
            return size;
        }
        pbuf += size;
        all_recv += size;
        return size;
    }

    bool is_file_data_receive_state() {
        return state == RECV_FILE && stateFile == FILE_DATA && fd;
    }
 
    ~Connection() {
        fprintf(stderr, "All receive %d bytes.\n", all_recv);
        close(csocket);
        free(buf);
        free(cwd);
    }
};
 
vector<Connection*> connections;

void close_connectoin_correct(int id, struct sockaddr_in addr, socklen_t addr_size) {
    int sd = connections[id]->csocket;
    getpeername(sd, (struct sockaddr*)&addr, &addr_size);
    fprintf(stderr, "Host disconnected successfully, ip: %s, port: %d\n",
        inet_ntoa(addr.sin_addr), ntohs(addr.sin_port)); 

    delete connections[id];
    connections[id] = NULL;
}

void close_connection_incorrect(int id, struct sockaddr_in addr, socklen_t addr_size) {
    int sd = connections[id]->csocket;
    getpeername(sd, (struct sockaddr*)&addr, &addr_size);
    fprintf(stderr, "Host disconnected with error, ip: %s, port: %d\n",
        inet_ntoa(addr.sin_addr), ntohs(addr.sin_port)); 

    delete connections[id];
    connections[id] = NULL; 
}
 
int main(int argc, char *argv[])
{      
    int master_socket = 0;
    int csocket_fd = 0;
    struct sockaddr_in addr;
    struct sockaddr_in s_addr;
    socklen_t addr_size = 0;
    unsigned short port = 0;  
   
    bzero(&addr, sizeof(struct sockaddr_in));
    bzero(&s_addr, sizeof(struct sockaddr_in));
    addr_size = sizeof(struct sockaddr_in);    
 
    port = PORT;
    s_addr.sin_family = PF_INET;
    s_addr.sin_port = htons(port);
    s_addr.sin_addr.s_addr = htonl(INADDR_ANY);
 
    master_socket = socket(PF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(master_socket, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt));
    bind(master_socket, (struct sockaddr *) &s_addr, sizeof(struct sockaddr_in));
    listen(master_socket, SOMAXCONN);      
    
    fprintf(stderr, "Waiting connections...\n"); 
    while (true)
    {
        fd_set readfds; 
        int max_desc;
        FD_ZERO(&readfds);
 
        FD_SET(master_socket, &readfds);
       
        max_desc = master_socket;
        for (int i = 0; i < connections.size(); ++i) {
            if (connections[i] != NULL) {
                int sd = connections[i]->csocket;
                if (sd) {
                    FD_SET(sd, &readfds);
                    if(sd > max_desc)
                        max_desc = sd;
                }                    
                if (connections[i]->is_file_data_receive_state()) {
                    FD_SET(connections[i]->fd, &readfds);
                    if (connections[i]->fd > max_desc)
                        max_desc = connections[i]->fd;
                }
            }            
        }
        
 
        int activity = select(max_desc + 1, &readfds, NULL, NULL, NULL);
        if (activity < 0)
            continue;

        if (FD_ISSET(master_socket, &readfds)) {
            int new_socket = accept(master_socket, (struct sockaddr *)&addr, &addr_size);
            fprintf(stderr, "New connection, socket fd is %d, ip is : %s, port : %d\n",
                new_socket, inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
           
            bool was_added = false;
            for (int i = 0; i < connections.size(); ++i) {
                if (connections[i] != NULL)
                    continue;
                connections[i] = new Connection(new_socket);
                was_added = true;
                break;
            }
            if (!was_added) {
                connections.push_back(new Connection(new_socket));
            }
        }
 
        for (int i = 0; i < connections.size(); ++i){
            if (connections[i] == NULL)
                continue;

            if (FD_ISSET(connections[i]->csocket, &readfds)) {
                int res = connections[i]->recv_to_buf();
                if (res <= 0){
                    if (res < 0)
                        perror("receive");

                    while ((res = connections[i]->do_what_state()) != 1) {
                        if (res == -1) 
                            close_connection_incorrect(i, addr, addr_size);                        
                    }

                    close_connectoin_correct(i, addr, addr_size);
                    continue;
                }  
            }  

            bool is_data_receiving = connections[i]->is_file_data_receive_state();
            if (is_data_receiving && FD_ISSET(connections[i]->fd, &readfds) || !is_data_receiving) 
            {
                int res = connections[i]->do_what_state(); 

                if (res == -1 || connections[i]->state == RECV_ERR_STOP) 
                    close_connection_incorrect(i, addr, addr_size);                                                
                if (connections[i]->state == RECV_STOP) 
                    close_connection_incorrect(i, addr, addr_size);
            }
        }
    }        
 
    close(master_socket);          
    return EXIT_SUCCESS;
}