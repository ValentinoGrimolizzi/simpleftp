#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <unistd.h>
#include <err.h>
#include <netinet/in.h>

#define BUFSIZE 512
#define CMDSIZE 4
#define PARSIZE 100

#define MSG_220 "220 srvFtp version 1.0\r\n"
#define MSG_331 "331 Password required for %s\r\n"
#define MSG_230 "230 User %s logged in\r\n"
#define MSG_530 "530 Login incorrect\r\n"
#define MSG_221 "221 Goodbye\r\n"
#define MSG_550 "550 %s: no such file or directory\r\n"
#define MSG_299 "299 File %s size %ld bytes\r\n"
#define MSG_226 "226 Transfer complete\r\n"

bool recv_cmd(int sd, char *operation, char *param) {
    char buffer[BUFSIZE], *token;
    int recv_s;

    // receive the command in the buffer and check for errors
    recv_s = recv(sd, buffer, BUFSIZE, 0);
    if (recv_s < 0) {
        warn("error receiving data");
        return false;
    }
    if (recv_s == 0) {
        errx(1, "connection closed by client");
        return false;
    }

    // expunge the terminator characters from the buffer
    buffer[strcspn(buffer, "\r\n")] = 0;

    // complex parsing of the buffer
    token = strtok(buffer, " ");
    if (token == NULL || strlen(token) < 4) {
        warn("not valid ftp command");
        return false;
    } else {
        if (operation[0] == '\0') strcpy(operation, token);
        if (strcmp(operation, token) != 0) {
            warn("abnormal client flow: did not send %s command", operation);
            return false;
        }
        token = strtok(NULL, " ");
        if (token != NULL) strcpy(param, token);
    }
    return true;
}

bool send_ans(int sd, char *message, ...) {
    char buffer[BUFSIZE];
    va_list args;
    va_start(args, message);
    vsprintf(buffer, message, args);
    va_end(args);

    // send answer preformatted and check errors
    if (send(sd, buffer, strlen(buffer), 0) < 0) {
        warn("error sending data");
        return false;
    }
    return true;
}

void retr(int sd, char *file_path) {
    FILE *file;
    int bread;
    long fsize;
    char buffer[BUFSIZE];

    // check if file exists if not inform error to client
    if ((file = fopen(file_path, "r")) == NULL) {
        send_ans(sd, MSG_550, file_path);
        return;
    }

    // send a success message with the file length
    fseek(file, 0, SEEK_END);
    fsize = ftell(file);
    rewind(file);
    send_ans(sd, MSG_299, file_path, fsize);

    // important delay for avoid problems with buffer size
    sleep(1);

    // send the file
    while ((bread = fread(buffer, sizeof(char), BUFSIZE, file)) > 0) {
        if (send(sd, buffer, bread, 0) < 0) {
            warn("error sending file data");
            fclose(file);
            return;
        }
    }

    // close the file
    fclose(file);

    // send a completed transfer message
    send_ans(sd, MSG_226);
}

bool check_credentials(char *user, char *pass) {
    FILE *file;
    char *path = "./ftpusers", *line = NULL, credentials[100];
    size_t line_size = 0;
    bool found = false;

    // make the credential string
    sprintf(credentials, "%s:%s", user, pass);

    // check if ftpusers file it's present
    if ((file = fopen(path, "r")) == NULL) {
        warn("Error opening %s", path);
        return false;
    }

    // search for credential string
    while (getline(&line, &line_size, file) != -1) {
        strtok(line, "\n");
        if (strcmp(line, credentials) == 0) {
            found = true;
            break;
        }
    }

    // close file and release any pointers if necessary
    fclose(file);
    if (line) free(line);

    // return search status
    return found;
}

bool authenticate(int sd) {
    char user[PARSIZE], pass[PARSIZE];

    // wait to receive USER action
    if (!recv_cmd(sd, "USER", user)) {
        send_ans(sd, MSG_530);
        return false;
    }
    send_ans(sd, MSG_331, user);

    // ask for password
    if (!recv_cmd(sd, "PASS", pass)) {
        send_ans(sd, MSG_530);
        return false;
    }

    // if credentials don't check denied login
    if (!check_credentials(user, pass)) {
        send_ans(sd, MSG_530);
        return false;
    }

    // confirm login
    send_ans(sd, MSG_230, user);
    return true;
}

void operate(int sd) {
    char op[CMDSIZE], param[PARSIZE];

    while (true) {
        op[0] = param[0] = '\0';
        // check for commands send by the client if not inform and exit
        if (!recv_cmd(sd, op, param)) {
            send_ans(sd, MSG_221);
            break;
        }

        if (strcmp(op, "RETR") == 0) {
            retr(sd, param);
        } else if (strcmp(op, "QUIT") == 0) {
            // send goodbye and close connection
            send_ans(sd, MSG_221);
            close(sd);
            break;
        } else {
            // invalid command
            // future use
            warn("Invalid command received");
        }
    }
}

int main(int argc, char *argv[]) {
    // arguments checking
    if (argc < 2) {
        errx(1, "Port expected as argument");
    } else if (argc > 2) {
        errx(1, "Too many arguments");
    }

    // reserve sockets and variables space
    int master_sd, slave_sd;
    struct sockaddr_in master_addr, slave_addr;
    socklen_t slave_len = sizeof(slave_addr);

    // create server socket and check errors
    master_sd = socket(AF_INET, SOCK_STREAM, 0);
    if (master_sd < 0) {
        err(1, "socket creation failed");
    }

    // bind master socket and check errors
    memset(&master_addr, 0, sizeof(master_addr));
    master_addr.sin_family = AF_INET;
    master_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    master_addr.sin_port = htons(atoi(argv[1]));

    if (bind(master_sd, (struct sockaddr *)&master_addr, sizeof(master_addr)) < 0) {
        err(1, "bind failed");
    }

    // make it listen
    if (listen(master_sd, 5) < 0) {
        err(1, "listen failed");
    }

    // main loop
    while (true) {
        // accept connections sequentially and check errors
        slave_sd = accept(master_sd, (struct sockaddr *)&slave_addr, &slave_len);
        if (slave_sd < 0) {
            warn("accept failed");
            continue;
        }

        // send hello
        send_ans(slave_sd, MSG_220);

        // operate only if authenticate is true
        if (authenticate(slave_sd)) {
            operate(slave_sd);
        } else {
            close(slave_sd);
        }
    }

    // close server socket
    close(master_sd);
    return 0;
}
