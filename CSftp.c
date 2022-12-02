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

/*
** Base server code adapted from https://beej.us/guide/bgnet/html/split/client-server-background.html#a-simple-stream-server
*/

// #define PORT "3490"  // the port users will be connecting to

#define BACKLOG 10   // how many pending connections queue will hold
#define MAX_NUM_THREADS 10 // max number of child thread connections allowed
#define BUFFER_SIZE 4096

char initialDir[BUFFER_SIZE];

enum FTP_CMD {INVALID = -1, USER, QUIT, CWD, CDUP, TYPE, MODE, STRU, RETR, PASV, NLST};

int user(int fd, char *userid);
int quit();
int cwd(int fd, char *path);
int cdup(int fd, char *initdir);
int type(int fd, char *rtype);
int mode(int fd, char *tmode);
int stru(int fd, char *fs);
int retr(char *filename);
int pasv();
int nlst(char *path);

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

// Multi-threading parameter passing from https://hpc-tutorials.llnl.gov/posix/example_code/hello_arg2.c
struct thread_data {
    int thread_id;
    int new_fd;
};

struct thread_data thread_data_array[MAX_NUM_THREADS];

void *command_handler(void *threadarg)
{
  int thread_id;
  int new_fd;
  struct thread_data *my_data;
  my_data = (struct thread_data *) threadarg;
  thread_id = my_data->thread_id;
  new_fd = my_data->new_fd;

  char buf[BUFFER_SIZE];
  ssize_t read_size;
  while((read_size = recv(new_fd , buf, BUFFER_SIZE - 1 , 0 )) > 0 ) {
    buf[read_size] = '\0';

    // test code to echo back to client
    // implement parsing of the buffer into commands and arguments here
    char command;
    char *argument; // TODO parse
    // put big switch statement
    char response[BUFFER_SIZE];
    // TODO implement commands
    switch(command) {
        case USER:
            user(new_fd, argument);
            break;
        case QUIT:
        // maybe if quit cmd, return -1 for exit(0)
            quit(new_fd);
            break;
        case CWD:
            cwd(new_fd, argument);
            break;
        case CDUP:
            getcwd(initialDir, BUFFER_SIZE);
            cdup(new_fd, initialDir);
            break;
        case TYPE:
            type(new_fd, argument);
            break;
        case MODE:
            mode(new_fd, argument);
            break;
        case STRU:
            stru(new_fd, argument);
            break;
        case RETR:
            retr(argument);
            break;
        case PASV:
            pasv();
            break;
        case NLST:
            nlst(argument);
            break;
        default:
            strcpy(response, "500 Syntax error, command unrecognized.\n");
            if (send(new_fd, response, sizeof(response), 0)) {
                perror("send");
            }
            break;
    }
    // https://canvas.ubc.ca/courses/101882/pages/tutorial-10-c-server-programming?module_item_id=5116919
    printf("%s", buf);
    char echo[BUFFER_SIZE + 6] = "echo: ";
    strcat(echo, buf);
    if (send(new_fd, echo, strlen(echo), 0) == -1)
        perror("send");
  }
  

  close(new_fd);
  printf("%s\n", "finished child thread");
  pthread_exit(NULL);
}

int user(int fd, char *userid) {
    char response[BUFFER_SIZE];

    if (strcmp(userid, "cs317") == 0) {
        strcpy(response, "230 User logged in, proceed.\n");
    } else {
        strcpy(response, "530 Not logged in.\n");
    }

    if (send(fd, response, strlen(response), 0) == -1) {
        perror("send");
    }
    return 0;
}

int quit(int fd) {
    char response[BUFFER_SIZE];
    strcpy(response, "221 Goodbye!");

    if (send(fd, response, strlen(response), 0) == -1) {
        perror("send");
    }
    
    close(fd);
    return -1;
}

int cwd(int fd, char *path) {
    char response[BUFFER_SIZE];

    if (path == NULL) {
        strcpy(response, "550 Requested action not taken. Path cannot be empty or null.\n");
    }
    if (strcmp(path, "./") == 0 || strcmp(path, "../") == 0) {
        strcpy(response, "550 Requested action not taken. Path cannot be ./ or ../.\n");
    }

    // TODO

    if (send(fd, response, strlen(response), 0) == -1) {
        perror("send");
    }
    return 0;
}

int cdup(int fd, char *initdir) {
    return 0;
}

int type(int fd, char *rtype) {
    char response[BUFFER_SIZE];

    if (strcasecmp(rtype, "I") == 0 || strcasecmp(rtype, "A") == 0) {
        strcpy(response, "200 Command okay.\n");
    } else {
        strcpy(response, "501 Syntax error in parameters or argument.\n");
    }

    if (send(fd, response, strlen(response), 0) == -1) {
        perror("send");
    }
    return 0;
}

int mode(int fd, char *tmode) {
    char response[BUFFER_SIZE];

    if (strcasecmp(tmode, "S") == 0) {
        strcpy(response, "200 Command okay.\n");
    } else if (strcasecmp(tmode, "B") == 0 || strcasecmp(tmode, "C") == 0 || strcasecmp(tmode, "Z") == 0) {
        strcpy(response, "Command not implemented for that parameter.\n");
    } else {
        strcpy(response, "501 Syntax error in parameters or argument.\n");
    }

    if (send(fd, response, strlen(response), 0) == -1) {
        perror("send");
    }
    return 0;
}

int stru(int fd, char *fs) {
    char response[BUFFER_SIZE];

    if (strcasecmp(fs, "F") == 0) {
        strcpy(response, "200 Command okay.\n");
    } else if (strcasecmp(fs, "R") == 0 || strcasecmp(fs, "P") == 0) {
        strcpy(response, "504 Command not implemented for that parameter.\n");
    } else {
        strcpy(response, "501 Syntax error in parameters or argument.\n");
    }

    if (send(fd, response, strlen(response), 0) == -1) {
        perror("send");
    }
    return 0;
}

int retr(char *filename) {
    return 0;
}

int pasv() {
    return 0;
}

int nlst(char *path) {
    return 0;
}

int main(int argc, char **argv)
{
    int sockfd, new_fd;  // listen on sock_fd, new connection on new_fd
    struct addrinfo hints, *servinfo, *p;
    struct sockaddr_storage their_addr; // connector's address information
    socklen_t sin_size;
    struct sigaction sa;
    int yes=1;
    char s[INET6_ADDRSTRLEN];
    int rv;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // use my IP

    // Checks if the program is run with anything other than an additional argument
    // Displays a usage method if so
    if (argc != 2) {
      usage(argv[0]);
      return -1;
    }

    // Gets address info using the port passed in, info then used to bind to socket later
    if ((rv = getaddrinfo(NULL, argv[1], &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    // loop through all the results and bind to the first we can
    for(p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype,
                p->ai_protocol)) == -1) {
            perror("server: socket");
            continue;
        }

        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
                sizeof(int)) == -1) {
            perror("setsockopt");
            exit(1);
        }

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            perror("server: bind");
            continue;
        }

        break;
    }

    freeaddrinfo(servinfo); // all done with this structure

    if (p == NULL)  {
        fprintf(stderr, "server: failed to bind\n");
        exit(1);
    }

    if (listen(sockfd, BACKLOG) == -1) {
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
            perror("accept");
            continue;
        }

        inet_ntop(their_addr.ss_family,
            get_in_addr((struct sockaddr *)&their_addr),
            s, sizeof s);
        printf("server: got connection from %s\n", s);

        thread_data_array[current_thread_id].thread_id = current_thread_id;
        thread_data_array[current_thread_id].new_fd = new_fd;
        current_thread_count++;
        pthread_create(&threads[current_thread_id], NULL, command_handler, (void *) &thread_data_array[current_thread_id]);

        pthread_join(threads[current_thread_id], NULL);
        printf("%s\n", "end of main while loop");
        close(new_fd);  // parent doesn't need this
        current_thread_id++;
        current_thread_count--; 
        
        // exit(0); // call this from somewhere if we need to quit the server
    }

    printf("%s\n", "out of main while loop end of main");
    return 0;
}


// // Here is an example of how to use the above function. It also shows
// // one how to get the arguments passed on the command line.
// /* this function is run by the second thread */

// void *inc_x()
// {
//   printf("x increment finished\n");
//   return NULL;
// }

// int main(int argc, char **argv) {

//     // This is some sample code feel free to delete it
//     // This is the main program for the thread version of nc

//     int i;
//     pthread_t child;
//     pthread_create(&child, NULL, inc_x, NULL);
    
//     // Check the command line arguments
//     if (argc != 2) {
//       usage(argv[0]);
//       return -1;
//     }

//     // This is how to call the function in dir.c to get a listing of a directory.
//     // It requires a file descriptor, so in your code you would pass in the file descriptor 
//     // returned for the ftp server's data connection
    
//     printf("Printed %d directory entries\n", listFiles(1, "."));
//     return 0;

// }
