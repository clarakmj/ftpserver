#include "dir.h"
#include "usage.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <ctype.h>

// #define PORT "3490"  // the port users will be connecting to

#define BACKLOG 10   // how many pending connections queue will hold
#define MAX_NUM_THREADS 10 // max number of child thread connections allowed
#define BUFFER_SIZE 4096

int pasvOn = 0;
int command_fd, pasvfd, data_fd;
char initialDir[BUFFER_SIZE];

enum FTP_CMD {INVALID = -1, USER, QUIT, CWD, CDUP, TYPE, MODE, STRU, RETR, PASV, NLST};

void user(int fd, char *userid, int* status);
void quit();
void cwd(int fd, char *path);
void cdup(int fd, char *initdir);
void type(int fd, char *rtype);
void mode(int fd, char *tmode);
void stru(int fd, char *fs);
void retr(int fd, char *filename);
void pasv();
void nlst(int fd);

void send_response(char responseBuf[], char message[], size_t size, int new_fd) {
    strcpy(responseBuf, message);
    if (send(new_fd, responseBuf, size, 0)) { 
        perror("send\n");
    }
}

void sigchld_handler(int s)
{
    // waitpid() might overwrite errno, so we save and restore it:
    int saved_errno = errno;

    while(waitpid(-1, NULL, WNOHANG) > 0);

    errno = saved_errno;
}


// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

// https://stackoverflow.com/questions/2371910/how-to-get-the-port-number-from-struct-addrinfo-in-unix-c
// get port, IPv4 or IPv6:
in_port_t get_in_port(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
        return (((struct sockaddr_in*)sa)->sin_port);

    return (((struct sockaddr_in6*)sa)->sin6_port);
}

void return_addr(struct sockaddr *sa, char* pasvResp)
{
    // https://www.geeksforgeeks.org/c-program-display-hostname-ip-address/
    char hostbuffer[256];
    char *IPbuffer;
    struct hostent *host_entry;
    int hostname;
  
    // To retrieve hostname
    hostname = gethostname(hostbuffer, sizeof(hostbuffer));
  
    // To retrieve host information
    host_entry = gethostbyname(hostbuffer);
  
    // To convert an Internet network
    // address into ASCII string
    IPbuffer = inet_ntoa(*((struct in_addr*)
                           host_entry->h_addr_list[0]));

    in_port_t port = ntohs(get_in_port(sa));

    char delim[] =".";
    unsigned count = 0;
    char *token = strtok(IPbuffer,delim);
    count++;

    char partialMsg[] = "227 Entering Passive Mode ("; 
    strcpy(pasvResp, partialMsg);
    char comma[] = ",";
    char bracket[] = ")";
    char newline[] = "\n";
    char period[] = ".";
    while(token != NULL) {
        strcat(pasvResp, token);
        strcat(pasvResp, comma);
        token = strtok(NULL,delim);
        count++;
    }
    uint16_t topBits = port >> 8;
    uint16_t bottomBits = port & 0xFF;
    char topBitChar[4];
    char bottomBitChar[4];
    sprintf(topBitChar, "%d", topBits);
    sprintf(bottomBitChar, "%d", bottomBits);
    strcat(pasvResp, topBitChar);
    strcat(pasvResp, comma);
    strcat(pasvResp, bottomBitChar);
    strcat(pasvResp, bracket);
    strcat(pasvResp, period);
    strcat(pasvResp, newline);
    // printf("%x\n", pasvResp[strlen(pasvResp)-1]);
    // printf("%d\n", strlen(pasvResp));
}

// Multi-threading parameter passing from https://hpc-tutorials.llnl.gov/posix/example_code/hello_arg2.c
struct thread_data {
    int thread_id;
    int new_fd;
};

struct thread_data thread_data_array[MAX_NUM_THREADS];

// Base server code adapted from https://beej.us/guide/bgnet/html/split/client-server-background.html#a-simple-stream-server
void createConnection(int* sockfd, char* initialPortChar, char* binded_info) {
    struct addrinfo hints, *servinfo, *p;
    struct sigaction sa;
    int yes=1;
    int rv;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // use my IP

    long temp = atol(initialPortChar);
    unsigned int initialPort = (unsigned int)temp;

    for (int i = initialPort; i < 8888; i++) {
        char port[8];
        sprintf(port, "%d", i);
        // Gets address info using the port passed in, info then used to bind to socket later
        if ((rv = getaddrinfo(NULL, port, &hints, &servinfo)) != 0) {
            fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
            return;
        }

        int binded = -1;

        // loop through all the results and bind to the first we can
        for(p = servinfo; p != NULL; p = p->ai_next) {
            if ((*sockfd = socket(p->ai_family, p->ai_socktype,
                    p->ai_protocol)) == -1) {
                perror("server: socket");
                continue;
            }
            if (setsockopt(*sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
                    sizeof(int)) == -1) {
                perror("setsockopt");
                exit(1);
            }
            if ((binded = bind(*sockfd, p->ai_addr, p->ai_addrlen)) == -1) {
                // close(*sockfd);
                perror("server: bind");
                continue;
            }
            break;
        }

        if (binded != -1) {
            break;
        }

    }


    printf("binded port is %d\n", ntohs(get_in_port((struct sockaddr *)p->ai_addr)));
    return_addr(p->ai_addr, binded_info);
 
    freeaddrinfo(servinfo); // all done with this structure

    if (p == NULL)  {
        fprintf(stderr, "server: failed to bind\n");
        exit(1);
    }

    if (listen(*sockfd, BACKLOG) == -1) {
        perror("listen");
        exit(1);
    }

    sa.sa_handler = sigchld_handler; // reap all dead processes
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }
}

// https://canvas.ubc.ca/courses/101882/pages/tutorial-10-c-server-programming?module_item_id=5116919
void *command_handler(void *threadarg)
{
    // State
    char buf[BUFFER_SIZE];
    int loggedIn = 0;
    int thread_id;
    struct thread_data *my_data;
    my_data = (struct thread_data *) threadarg;
    thread_id = my_data->thread_id;
    getcwd(initialDir, BUFFER_SIZE);

    // Command connection variables
    command_fd = my_data->new_fd;

    // Data Variables
    pthread_t data_thread;

    // Send 220 message. Ready for login
    char readyMsg[] = "220 Service ready for new user.\n";
    if (send(command_fd, readyMsg, sizeof(readyMsg), 0)) { 
                perror("send\n");
    } 

    ssize_t read_size;
    while((read_size = recv(command_fd , buf, BUFFER_SIZE - 1 , 0 )) > 0 ) 
    {
        buf[read_size] = '\0';

        enum FTP_CMD command = INVALID;
        char argument[BUFFER_SIZE];
        char response[BUFFER_SIZE];

        // https://linuxhint.com/split-strings-delimiter-c/
        char delim[] =" \t\r\n\v\f";
        unsigned count = 0;
        char *token = strtok(buf,delim);
        count++;
        while(token != NULL) {
            // Count 1 - is the command
            if (count == 1) {
                int j = 0;
                char ch;
            
                while (token[j]) {
                    ch = token[j];
                    token[j] = toupper(ch);
                    j++;
                }

                // parse this first string for a command match
                if (strcmp(token, "USER") == 0) {
                    command = USER;
                } else if (strcmp(token, "QUIT") == 0) {
                    command = QUIT;
                } else if (strcmp(token, "CWD") == 0) {
                    command = CWD;
                } else if (strcmp(token, "CDUP") == 0) {
                    command = CDUP;
                } else if (strcmp(token, "TYPE") == 0) {
                    command = TYPE;
                } else if (strcmp(token, "MODE") == 0) {
                    command = MODE;
                } else if (strcmp(token, "STRU") == 0) {
                    command = STRU;
                } else if (strcmp(token, "RETR") == 0) {
                    command = RETR;
                } else if (strcmp(token, "PASV") == 0) {
                    command = PASV;
                } else if (strcmp(token, "NLST") == 0) {
                    command = NLST;
                }
            }
            if (count == 2) {
                // is empty string
                if (strlen(token) == 0) {
                    send_response(response, "500 Syntax error, command unrecognized.\n", sizeof(response), command_fd);
                }
                // otherwise set argument
                strcpy(argument, token);
            }
            if (count > 2) {
                // return error of too many arguments
                send_response(response, "501 Syntax error in parameters or arguments.\n", sizeof(response), command_fd);
            }
            printf("Token no. %d : %s \n", count, token);
            token = strtok(NULL,delim);
            count++;
        }

        switch(command) {
            // USER <SP> <username> <CRLF>
            case USER:
                if (strlen(argument) <= 0) {
                    send_response(response, "501 Syntax error in parameters or arguments.\n", sizeof(response), command_fd);
                    break;
                }
                user(command_fd, argument, &loggedIn);
                break;
            // QUIT <CRLF>
            case QUIT:
            // maybe if quit cmd, return -1 for exit(0)
                quit(command_fd);
                break;
            // CWD  <SP> <pathname> <CRLF>
            case CWD:
                if (loggedIn != 1) {
                    send_response(response, "530 Not logged in.\n", sizeof(response), command_fd);
                    break;
                }
                if (strlen(argument) <= 0) {
                    send_response(response, "501 Syntax error in parameters or arguments.\n", sizeof(response), command_fd);
                    break;
                }
                cwd(command_fd, argument);
                break;
            // CDUP <CRLF>
            case CDUP:
            // TODO not sure where initialDir should be set
                if (loggedIn != 1) {
                    send_response(response, "530 Not logged in.\n", sizeof(response), command_fd);
                    break;
                }
                cdup(command_fd, initialDir);
                break;
            // TYPE <SP> <type-code> <CRLF>
            case TYPE:
                if (loggedIn != 1) {
                    send_response(response, "530 Not logged in.\n", sizeof(response), command_fd);
                    break;
                }
                if (strlen(argument) <= 0) {
                    send_response(response, "501 Syntax error in parameters or arguments.\n", sizeof(response), command_fd);
                    break;
                }
                type(command_fd, argument);
                break;
            // MODE <SP> <mode-code> <CRLF>
            case MODE:
                if (loggedIn != 1) {
                    send_response(response, "530 Not logged in.\n", sizeof(response), command_fd);
                    break;
                }
                if (strlen(argument) <= 0) {
                    send_response(response, "501 Syntax error in parameters or arguments.\n", sizeof(response), command_fd);
                    break;
                }
                mode(command_fd, argument);
                break;
            // STRU <SP> <structure-code> <CRLF>
            case STRU:
                if (loggedIn != 1) {
                    send_response(response, "530 Not logged in.\n", sizeof(response), command_fd);
                    break;
                }
                if (strlen(argument) <= 0) {
                    send_response(response, "501 Syntax error in parameters or arguments.\n", sizeof(response), command_fd);
                    break;
                }
                stru(command_fd, argument);
                break;
            // RETR <SP> <pathname> <CRLF>
            case RETR:
                if (loggedIn != 1) {
                    send_response(response, "530 Not logged in.\n", sizeof(response), command_fd);
                    break;
                }
                if (strlen(argument) <= 0) {
                    send_response(response, "501 Syntax error in parameters or arguments.\n", sizeof(response), command_fd);
                    break;
                }
                retr(command_fd, argument);
                break;
            // PASV <CRLF>
            case PASV:
                if (loggedIn != 1) {
                    send_response(response, "530 Not logged in.\n", sizeof(response), command_fd);
                    break;
                }
                pthread_create(&data_thread, NULL, (void*) pasv, NULL);
                break;
            // NLST [<SP> <pathname>] <CRLF>
            case NLST:
                if (loggedIn != 1) {
                    send_response(response, "530 Not logged in.\n", sizeof(response), command_fd);
                    break;
                }
                if (pasvOn != 1) {
                    send_response(response, "503 Bad sequence of commands.\n", sizeof(response), command_fd);
                    break;
                }
                if (strlen(argument) > 0) {
                    send_response(response, "501 Syntax error in parameters or arguments.\n", sizeof(response), command_fd);
                    break;
                }
                nlst(data_fd);
                break;
            default:
                send_response(response, "500 Syntax error, command unrecognized.\n", sizeof(response), command_fd);
                break;
        }

        // Clear the buffer after running a command
        memset(buf, '\0', sizeof(buf));
        memset(argument, '\0', sizeof(argument));
        memset(response, '\0', sizeof(response));
    }

    if (data_thread != NULL) {
        pthread_join(data_thread, NULL);
    }

    close(command_fd);
    printf("%s\n", "finished child thread");
    pthread_exit(NULL);
}

void user(int fd, char *userid, int* status) {
    char response[BUFFER_SIZE];
    if (strcmp(userid, "cs317") == 0) {
        strcpy(response, "230 User logged in, proceed.\n");
        *status = 1;
    } else {
        strcpy(response, "530 Not logged in.\n");
        *status = 0;
    }

    if (send(fd, response, strlen(response), 0) == -1) {
        perror("send\n");
    }

    // Clear response buffer
    memset(response, '\0', sizeof(response));
}

void quit(int fd) {
    char response[BUFFER_SIZE];
    strcpy(response, "221 Goodbye!");

    if (send(fd, response, strlen(response), 0) == -1) {
        perror("send\n");
    }
    
    close(fd);
    // Clear response buffer
    memset(response, '\0', sizeof(response));
}

void cwd(int fd, char *path) {
    char response[BUFFER_SIZE];
    char temp[BUFFER_SIZE];
    char *token;
    int performChdir = 1;

    if (path == NULL) {
        strcpy(response, "550 Requested action not taken. Path cannot be empty or null.\n");
    } else {
        strcpy(temp, path);
        token = strtok(path, "/\t\r\n\v\f");
        if (strcmp(token, ".") == 0 || strcmp(token, "..") == 0 || strcmp(token, "./") == 0 || strcmp(token, "../") == 0) {
            strcpy(response, "550 Requested action not taken. Path cannot be ./ or ../.\n");
            performChdir = 0;
        }
    }

    while (token != NULL) {
        if (strcmp(token, "..") == 0) {
            strcpy(response, "550 Requested action not taken. Path cannot contain ../.\n");
            performChdir = 0;
            break;
        }
        token = strtok(NULL, "/\n");
    }

    if (performChdir) {
        if (chdir(temp) == 0) {
            strcpy(response, "250 Requested file action okay, completed.\n");
        } else {
            strcpy(response, "550 Requested action not taken; file unavailable.");
        }
    }

    if (send(fd, response, strlen(response), 0) == -1) {
        perror("send\n");
    }

    // Clear response buffer
    memset(response, '\0', sizeof(response));
    memset(temp, '\0', sizeof(temp));
}

void cdup(int fd, char *initdir) {
    char response[BUFFER_SIZE];

    char currentDir[BUFFER_SIZE];
    getcwd(currentDir, BUFFER_SIZE);

    if (strcmp(initdir, currentDir) == 0) {
        strcpy(response, "550 Requested action not taken.\n");
    } else if (chdir("..") == 0) {
        strcpy(response, "200 Command okay.\n");
    } else {
        strcpy(response, "550 Requested action not taken.\n");
    }

    if (send(fd, response, strlen(response), 0) == -1) {
        perror("send\n");
    }

    // Clear response buffer
    memset(response, '\0', sizeof(response));
}

void type(int fd, char *rtype) {
    char response[BUFFER_SIZE];

    if (strcasecmp(rtype, "I") == 0 || strcasecmp(rtype, "A") == 0) {
        strcpy(response, "200 Command okay.\n");
    } else {
        strcpy(response, "501 Syntax error in parameters or argument.\n");
    }

    if (send(fd, response, strlen(response), 0) == -1) {
        perror("send\n");
    }

    // Clear response buffer
    memset(response, '\0', sizeof(response));
}

void mode(int fd, char *tmode) {
    char response[BUFFER_SIZE];

    if (strcasecmp(tmode, "S") == 0) {
        strcpy(response, "200 Command okay.\n");
    } else if (strcasecmp(tmode, "B") == 0 || strcasecmp(tmode, "C") == 0 || strcasecmp(tmode, "Z") == 0) {
        strcpy(response, "Command not implemented for that parameter.\n");
    } else {
        strcpy(response, "501 Syntax error in parameters or argument.\n");
    }

    if (send(fd, response, strlen(response), 0) == -1) {
        perror("send\n");
    }

    // Clear response buffer
    memset(response, '\0', sizeof(response));
}

void stru(int fd, char *fs) {
    char response[BUFFER_SIZE];

    if (strcasecmp(fs, "F") == 0) {
        strcpy(response, "200 Command okay.\n");
    } else if (strcasecmp(fs, "R") == 0 || strcasecmp(fs, "P") == 0) {
        strcpy(response, "504 Command not implemented for that parameter.\n");
    } else {
        strcpy(response, "501 Syntax error in parameters or argument.\n");
    }

    if (send(fd, response, strlen(response), 0) == -1) {
        perror("send\n");
    }

    // Clear response buffer
    memset(response, '\0', sizeof(response));
}

void retr(int fd, char *filename) {
    char response[BUFFER_SIZE];
    FILE *f;
    int dataRead;
    char buffer[BUFFER_SIZE];

    if (pasvOn == 0) {
        strcpy(response, "503 Bad sequence of commands.\n");
    } else {
        if (filename == NULL) {
            strcpy(response, "550 Requested action not taken; file unavailable.\n");
        } else {
            f = fopen(filename, "r");
            if (f == NULL) {
                strcpy(response, "550 Requested action not taken; file unavailable.\n");
            } else {
                strcpy(response, "150 File status okay; about to open data connection.\n");
                if (send(fd, response, strlen(response), 0) == -1) {
                    perror("send\n");
                }

                while ((dataRead = fread(buffer, sizeof(char), BUFFER_SIZE, f)) > 0) {
                    printf("Elements read: %d", dataRead);
                    if (write(data_fd, buffer, dataRead) < 0) {
                        printf("Error in sending data.\n");
                    }
                }
                memset(buffer, BUFFER_SIZE, sizeof(buffer));
            }
            fclose(f);
            pasvOn = 0;
            strcpy(response, "226 Closing data connection.\n");
            if (send(fd, response, strlen(response), 0) == -1) {
                perror("send\n");
            }
        }
    }

    // Clear response buffer
    memset(response, '\0', sizeof(response));
}




void pasv() {
    struct sockaddr_storage their_addr; // connector's address information
    socklen_t sin_size;
    char response[BUFFER_SIZE];
    char pasvResp[53];
    createConnection(&pasvfd, "8000", pasvResp);
    // get and send the listening port to the client
    send_response(response, pasvResp, strlen(pasvResp), command_fd);

    int rv; 
    struct pollfd ufds[1];
    ufds[0].fd = pasvfd;
    ufds[0].events = POLLIN | POLLPRI;
    rv = poll(ufds, 1, 30000);

    if (rv == 0) {
        char timeout[] = "425 Can't open data connection. No connection after 30s.\n";
        send_response(response, timeout, strlen(timeout), command_fd);
        memset(response, '\0', sizeof(response));
        close(pasvfd);
        pthread_exit(NULL);
        return;
    }

    // main accept() loop
    while(1) {  
        sin_size = sizeof their_addr;

        data_fd = accept(pasvfd, (struct sockaddr *)&their_addr, &sin_size);
        if (data_fd == -1) {
            perror("accept\n");
            continue;
        } else {
            break;
        } 
    }

    pasvOn = 1;

    // Clear response buffer
    memset(response, '\0', sizeof(response));
    memset(pasvResp, '\0', sizeof(pasvResp));
}

void nlst(int fd) {
    char cwd[BUFFER_SIZE];
    char response[BUFFER_SIZE];

    getcwd(cwd, BUFFER_SIZE);
    printf("Printed %d directory entries\n", listFiles(fd, cwd));
    char msg[] = "150 File status okay; about to open data connection.\n";
    send_response(response, msg, sizeof(msg), command_fd);

    // Clear buffers
    memset(response, '\0', sizeof(response));
    memset(cwd, '\0', sizeof(cwd));
}

int main(int argc, char **argv)
{
    int sockfd, new_fd;  // listen on sock_fd, new connection on new_fd
    struct sockaddr_storage their_addr; // connector's address information
    socklen_t sin_size;
    char s[INET6_ADDRSTRLEN];
    char binded_info[26];

    // Checks if the program is run with anything other than an additional argument
    // Displays a usage method if so
    if (argc != 2) {
      usage(argv[0]);
      return -1;
    }

    createConnection(&sockfd, argv[1], binded_info);

    printf("server: waiting for connections...\n");

    pthread_t threads[MAX_NUM_THREADS];
    int current_thread_id = 0;
    int current_thread_count = 0;

    while(1) {  // main accept() loop
        sin_size = sizeof their_addr;

        if (current_thread_count > MAX_NUM_THREADS) {
            printf("server: exceeded max number of connections\n");
            continue;
        }

        new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
        if (new_fd == -1) {
            perror("accept\n");
            continue;
        } else {
            inet_ntop(their_addr.ss_family,
                get_in_addr((struct sockaddr *)&their_addr),
                s, sizeof s);
            printf("server: got connection from %s\n", s);

            thread_data_array[current_thread_id].thread_id = current_thread_id;
            thread_data_array[current_thread_id].new_fd = new_fd;
            current_thread_count++;
            pthread_create(&threads[current_thread_id], NULL, command_handler, (void *) &thread_data_array[current_thread_id]);

            printf("%s\n", "end of main while loop");
            current_thread_id++;
            // current_thread_count--; 
        }
        
        // exit(0); // call this from somewhere if we need to quit the server
    }

    for (int i = 0; i <= current_thread_id; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("%s\n", "out of main while loop end of main");
    return 0;
}
