#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <assert.h>

#define GROWTH_MULT     2
#define BLACKLOG      100
#define MAX_PLAYERS     2
#define BUFFER_SIZE 16384
#define INVALID        -1
#define MAX_ROUNDS      4

#define NEXT_ROUND(R) (((R) % MAX_ROUNDS) + 1)

#define ERROR_USAGE  "USAGE: <program> <ip> <port>"
#define ERROR_OPEN   "opening socket"
#define ERROR_BIND   "bind"
#define ERROR_ACCEPT "accept"
#define ERROR_READ   "read"
#define ERROR_WRITE  "write"
#define ERROR_REOPEN "reopen"
#define ERROR_LISTEN "listen"
#define ERROR_SELECT "select"

#define INIT_HTML             "init.html"
#define MENU_START_HEAD_HTML  "menu_start_head.html"
#define MENU_START_TAIL_HTML  "menu_start_tail.html"
#define MENU_WIN_HTML         "menu_win.html"
#define PLAY_FIRST_HTML       "play_first.html"
#define PLAY_DISCARD_HTML     "play_discard.html"
#define PLAY_TAIL_HTML        "play_tail.html"
#define PLAY_ACCEPT_HEAD_HTML "play_accept_head.html"
#define PLAY_ACCEPT_BODY_HTML "play_accept_body.html"
#define PLAY_ACCEPT_TAIL_HTML "play_accept_tail.html"
#define QUIT_HTML             "quit.html"

/* note: cookie Expiration date is the last time before the next Unix epoch*/
#define HTTP_200 "HTTP/1.1 200 OK\r\n" \
                 "Connection: keep-alive\r\n" \
                 "Content-Type: text/html\r\n" \
                 "Content-Length: %d\r\n" \
                 "\r\n"


/* note: cookie Expiration date is the last time before the next Unix epoch*/
#define HTTP_200_COOKIE "HTTP/1.1 200 OK\r\n" \
                 "Connection: keep-alive\r\n" \
                 "Content-Type: text/html\r\n" \
                 "Content-Length: %d\r\n" \
                 "Set-Cookie: user_name=%s; Expires=Tue, 19 Jan 2038 03:14:07 GMT\r\n" \
                 "\r\n"



#define HTTP_404 "HTTP/1.1 404 NOT FOUND\r\n" \
                 "Content-Length: 0\r\n" \
                 "\r\n"

#define HTTP_400 "HTTP/1.1 400 BAD REQUEST\r\n" \
                 "Content-Length: 0\r\n" \
                 "\r\n"

#define FIELD_COOKIE  "user_name="
#define FIELD_KEYWORD "keyword="
#define FIELD_USER    "user="
#define FIELD_END     '\r'
#define FIELD_SEP     '&'

#define FAVICON "GET /favicon.ico"

typedef enum state { 
    INIT, 
    MENU, 
    PLAY, 
    QUIT
} State;

typedef struct player {
    int sockfd;
    State state;
    int round;
    char *user;
    char **guesses;
    int n_guesses, g_alloc;
} Player;

typedef struct game {
    Player player[MAX_PLAYERS];
    int n_players, won;
} Game;

void run(char *ip_str, char *port_str);
int init_server_socket(char *ip_str, int port_no);
void check_error(int error_num, char *error_str);
void send_http_200(Player *player, char *buff, int len);
void send_http_200_cookie(Player *player, char *buff, int len);
void send_http_head(int sockfd, char *head);

int buffer_file(char *filename, char *buff, int space);

void game_init(Game *game);

void process_request(Game *game, int player_index, char *buff);

void free_guesses(Game *game);
char *add_guess(Player *player, char *message);
int match_guess(Player *player, char *guess);

void game_quit(Game *game, Player *player, char *buff);
void game_won(Game *game, Player *player, char *buff);
void game_play(Game *game, Player *player, char *buff, char *head);
void game_accept(Game *game, Player *player, char *buff);
void game_menu(Game *game, Player *player, char *buff, int cookie);
int done(Game *game);

int read_cookie(Player *player, char *buff);
void add_cookie(Player *player, char *cookie);

int main(int argc, char **argv) {
    // validate command line args
    if (argc != 3) {
        fprintf(stderr, ERROR_USAGE);
        exit(EXIT_FAILURE);
    }

    run(argv[1], argv[2]);

    return 0;
}

void run(char *ip_str, char *port_str) {
    int server_sockfd = init_server_socket(ip_str, atoi(port_str));
    
    printf("image_tagger server is now running at IP: %s on port %s\n", 
           ip_str, port_str);

    int rv, maxfd = server_sockfd;
    fd_set sockfds;
    FD_SET(server_sockfd, &sockfds);

    char buff[BUFFER_SIZE + 1], ip[INET_ADDRSTRLEN + 1];
    buff[BUFFER_SIZE] = ip[INET_ADDRSTRLEN] = '\0';
    Game game;
    game_init(&game);
    Player *player;

    // loop forever
    while (1) {
        // restart game if both players have quitted
        if (done(&game)) {
            for (int i = 0; i < MAX_PLAYERS; i++) {
                player = &game.player[i];
                player->state = INIT;
                player->sockfd = INVALID;
                player->round = 0;
            }
            game.n_players = 0;
            game.won = INVALID;
        }

        fd_set readfds = sockfds; 

        rv = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        check_error(rv, ERROR_SELECT);

        for (int sockfd = 0, tmp_max = maxfd; sockfd <= tmp_max; sockfd++) {
            // skip if not set
            if (!FD_ISSET(sockfd, &readfds)) {
                continue;
            }

            // try accept a new conection
            if (sockfd == server_sockfd) {
                struct sockaddr_in cliaddr;
                socklen_t clilen = sizeof(cliaddr);
                int new_sockfd = accept(server_sockfd, 
                    (struct sockaddr *)&cliaddr, &clilen);
                check_error(new_sockfd, ERROR_ACCEPT);

                if (new_sockfd > maxfd) {
                    maxfd = new_sockfd;
                }
                FD_SET(new_sockfd, &sockfds);
                continue;
            }

            // try service a player's request
            int serviced = 0;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                player = &game.player[i];
                if (player->sockfd == sockfd) {
                    rv = read(sockfd, buff, BUFFER_SIZE);
                    check_error(rv, ERROR_READ);
                    buff[rv] = '\0';
                    process_request(&game, i, buff);

                    if (player->state == QUIT) {
                        player->sockfd = INVALID;
                        close(sockfd);
                        FD_CLR(sockfd, &sockfds);
                    }

                    serviced = 1;
                    break;
                }
            }
            if (serviced) {
                continue;
            }

            // try 404 if we have too many players :(
            if (game.n_players >= MAX_PLAYERS) {
                send_http_head(sockfd, HTTP_404);
                close(sockfd);
                FD_CLR(sockfd, &sockfds);
                continue;
            }

            // try accept a new player
            game.n_players++;
            // find a slot in game.player that is empty (INVALID sockfd)
            for (int i = 0; i < MAX_PLAYERS; i++) { 
                player = &game.player[i];
                if (player->sockfd == INVALID) {
                    rv = read(sockfd, buff, BUFFER_SIZE);
                    check_error(rv, ERROR_READ);
                    buff[rv] = '\0';

                    if (rv == 0) {
                        close(sockfd);
                        FD_CLR(sockfd, &sockfds);
                        break;
                    }

                    player->sockfd = sockfd;

                    // serve them their start screen
                    if (read_cookie(player, buff)) {
                        game_menu(&game, player, buff, 0);
                    } else {
                        int len = buffer_file(INIT_HTML, buff, BUFFER_SIZE);
                        send_http_200(player, buff, len);
                        player->state = INIT;
                    }
                    break;
                }
            }
        }
    }

    // cleanup
    free_guesses(&game);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        player = &game.player[i];
        free(player->guesses);
        free(player->user);
    }

    close(server_sockfd);
}

void check_error(int error, char *error_str) {
    // error values are negative
    if (error < 0) {
        perror(error_str);
        exit(EXIT_FAILURE);
    }
}

int init_server_socket(char *ip_str, int port) {
    // create tcp socket
    int server_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    check_error(server_sockfd, ERROR_OPEN);

    // allow for socket reuse
    int re = 1;
    int rv = setsockopt(server_sockfd, SOL_SOCKET, SO_REUSEADDR, 
        &re, sizeof(int));
    check_error(rv, ERROR_REOPEN);

    // set up address we are going to listen to 
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(ip_str);
    serv_addr.sin_port = htons(port);

    // bind socket
    rv = bind(server_sockfd, (struct sockaddr *) &serv_addr, 
        sizeof(serv_addr));
    check_error(rv, ERROR_BIND);

    // ready to listen
    rv = listen(server_sockfd, BLACKLOG);
    check_error(rv, ERROR_LISTEN);

    return server_sockfd;
}

void send_http_200(Player *player, char *buff, int len) {
    char head[BUFFER_SIZE + 1];
    int rv;

    int hlen = sprintf(head, HTTP_200, len);
    // send the header first
    rv = write(player->sockfd, head, hlen);
    check_error(rv, ERROR_WRITE);
    // send whatever is in the buffer
    rv = write(player->sockfd, buff, len);
    check_error(rv, ERROR_WRITE);
}

void send_http_200_cookie(Player *player, char *buff, int len) {
    char head[BUFFER_SIZE + 1];
    int rv;
    
    int hlen = sprintf(head, HTTP_200_COOKIE, len, player->user);

    // send the header first
    rv = write(player->sockfd, head, hlen);
    check_error(rv, ERROR_WRITE);
    // send whatever is in the buffer
    rv = write(player->sockfd, buff, len);
    check_error(rv, ERROR_WRITE);
}


void send_http_head(int sockfd, char *head) {
    int rv;

    rv = write(sockfd, head, strlen(head));
    check_error(rv, ERROR_WRITE);
}

void game_init(Game *game) {
    Player *player;

    game->won = INVALID;
    game->n_players = 0;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        player = &game->player[i];

        player->sockfd = INVALID;
        player->state = INIT;
        player->user = malloc(sizeof(char));
        assert(player->user);
        player->user[0] = '\0';
        player->guesses = malloc(sizeof(char *));
        player->g_alloc = 1;
        assert(player->guesses);
        player->n_guesses = 0;
        player->round = 0;
    }
}

void process_request(Game *game, int player_index, char *buff) {
    int other_index = 1 - player_index;

    Player *player = &game->player[player_index];
    Player *other = &game->player[other_index];

    char *message = strstr(buff, "\r\n\r\n") + 4, *user;

    // ignore favicon.ico requests
    if (!strncmp(FAVICON, buff, strlen(FAVICON))) {
        send_http_head(player->sockfd, HTTP_404);
        return;
    }

    if (strncmp("GET ", buff, 4) && strncmp("POST ", buff, 5)) {
        send_http_head(player->sockfd, HTTP_400);
        return;
    }

    // force quit if other player has
    if (other->state == QUIT) {
        game_quit(game, player, buff);
    } else {
        switch (player->state) {
            case INIT:
                user = strstr(message, FIELD_USER) + strlen(FIELD_USER);
                add_cookie(player, user);

                game_menu(game, player, buff, 1);
            break;

            case MENU:
                // pressed Start
                if (!strncmp("GET ", buff, 4)) {
                    player->round = NEXT_ROUND(player->round);
                    game_play(game, player, buff, PLAY_FIRST_HTML);
                // pressed Quit
                } else {
                    game_quit(game, player, buff);
                }
            break;

            case PLAY:
                // pressed quit
                if (strstr("quit=Quit", message)) {
                    game_quit(game, player, buff);

                // game has been won
                } else if (game->won == player_index) {
                    game_won(game, player, buff);
                    game->won = INVALID;

                // other isnt ready
                } else if (other->state != PLAY || 
                            other->round != player->round) {
                    game_play(game, player, buff, PLAY_DISCARD_HTML);

                // accept guess
                } else {
                    char *guess = add_guess(player, message);
                    if (guess && match_guess(other, guess)) {
                        game_won(game, player, buff);
                        game->won = other_index;
                    } else {    
                        game_accept(game, player, buff);
                    }
                }
            break;

            case QUIT:
            break;
        }
    }
}

int buffer_file(char *filename, char *buff, int space) {
    int filefd = open(filename, O_RDONLY);
    check_error(filefd, ERROR_OPEN);
    int n = read(filefd, buff, space);
    check_error(n, ERROR_READ);

    close(filefd);
    buff[n] = '\0';

    return n;
}

void free_guesses(Game *game) {
    for (int p = 0; p < MAX_PLAYERS; p++) {
        Player *player = &game->player[p];
        for (int i = 0; i < player->n_guesses; i++) {
            free(player->guesses[i]);
        }
        player->n_guesses = 0;
    }
}

char *add_guess(Player *player, char *message) {
    // find keyword in message
    char *keyword = strstr(message, FIELD_KEYWORD) + strlen(FIELD_KEYWORD);
    int klen = strchr(keyword, FIELD_SEP) - keyword;

    // ignore empty guesses
    if (klen <= 0) {
        return NULL;
    }

    // reallocate guess space if necessary
    if (player->n_guesses >= player->g_alloc) {
        player->guesses = realloc(player->guesses, 
            sizeof(char *) * (player->g_alloc *= GROWTH_MULT));
        assert(player->guesses);
    }
    // add keyword to guesses
    char *guess = malloc((klen + 1) * sizeof(char));
    assert(guess);
    strncpy(guess, keyword, klen);
    guess[klen] = '\0';
    player->guesses[player->n_guesses++] = guess;   

    return guess;
}

int match_guess(Player *player, char *guess) {
    for (int i = 0; i < player->n_guesses; i++) {
        if (!strcmp(guess, player->guesses[i])) {
            return 1;
        }
    }
    return 0;
}

void game_quit(Game *game, Player *player, char *buff) {
    free_guesses(game);

    int len = buffer_file(QUIT_HTML, buff, BUFFER_SIZE);
    send_http_200(player, buff, len);
    player->state = QUIT;
}

void game_won(Game *game, Player *player, char *buff) {
    free_guesses(game);

    int len = buffer_file(MENU_WIN_HTML, buff, BUFFER_SIZE);
    send_http_200(player, buff, len);
    player->state = MENU;
}

void game_play(Game *game, Player *player, char *buff, char *head) {
    int len = buffer_file(head, buff, BUFFER_SIZE);
    len += sprintf(buff + len, "%d", player->round);
    len += buffer_file(PLAY_TAIL_HTML, buff + len, BUFFER_SIZE - len);
    
    send_http_200(player, buff, len);

    player->state = PLAY;
}

void game_accept(Game *game, Player *player, char *buff) {
    int len = buffer_file(PLAY_ACCEPT_HEAD_HTML, buff, BUFFER_SIZE);
    len += sprintf(buff + len, "%d", player->round);
    len += buffer_file(PLAY_ACCEPT_BODY_HTML, buff + len, BUFFER_SIZE - len);

    for (int i = 0; i < player->n_guesses; i++) {
        len += sprintf(buff + len, "%s, ", player->guesses[i]);
    }

    len += buffer_file(PLAY_ACCEPT_TAIL_HTML, buff + len, BUFFER_SIZE - len);

    send_http_200(player, buff, len);
}

void game_menu(Game *game, Player *player, char *buff, int cookie) {
    int len = buffer_file(MENU_START_HEAD_HTML, buff, BUFFER_SIZE);
    len += sprintf(buff + len, "%s", player->user);
    len += buffer_file(MENU_START_TAIL_HTML, buff + len, BUFFER_SIZE - len);

    if (cookie) {
        send_http_200_cookie(player, buff, len);
    } else {
        send_http_200(player, buff, len);   
    }
    
    player->state = MENU;
}


int done(Game *game) {
    // check all players have quit
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *player = &game->player[i];
        if (player->state != QUIT && player->sockfd != INVALID) {
            return 0;
        }
    }
    return 1;
}

int read_cookie(Player *player, char *buff) {
    // find cookie in buff
    char *cookie = strstr(buff, FIELD_COOKIE);
    if (!cookie) {
        player->user[0] = '\0';
        return 0;
    }
    // add cookie to user
    cookie += strlen(FIELD_COOKIE);
    int clen = strchr(cookie, FIELD_END) - cookie;

    char c = cookie[clen];
    cookie[clen] = '\0';
    add_cookie(player, cookie);
    cookie[clen] = c;
    return 1;
}

void add_cookie(Player *player, char *cookie) {
    // overqrite the username with cookie
    int clen = strlen(cookie);
    player->user = realloc(player->user, (clen + 1) * sizeof(char));
    assert(player->user);
    strcpy(player->user, cookie);
}
