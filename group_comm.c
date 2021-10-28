#include <netinet/in.h>
#include <stdio.h> //printf
#include <string.h> //memset
#include <stdlib.h> //exit(0);
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h> // for open
#include <unistd.h> // for close
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <time.h>
#include <signal.h>
#include <dirent.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/time.h>
#include <pthread.h>
#include <sys/sendfile.h>
#include <errno.h>
#include <sys/time.h>

#define MAXGRPSIZE 50       //Total amount of groups formable in the LAN
#define MSGBUFSIZE 1000     //Max limit of the message which can be sent
#define GRPNAMESIZE 100     // Max limit of characters that can be used to
#define MAXIPSIZE 200       // Limit to how large can the IP Address storing string can be
#define MAXFILESYS 10       // limit to how many files records of the system can be stored
#define MAXFILENET 100     //limit to how many file records of the network can be stored in the system
#define MAXFILENAME 100     // limit to how long ca a file name be
#define MAXFILESIZE 50000   //The max limit of a size of file which can be downloaded/uploaded

#define PORT1 8888  // for multicast group msgs
#define PORT2 8889  // for system messages of create_group notifs
#define PORT3  8890 // for replies to search file and download of file

typedef struct Groups
{
    int group_id;
    char name[GRPNAMESIZE];
    char ip[MAXIPSIZE];

}group_info;

typedef struct packet
{
    char msg[MSGBUFSIZE];
    int init_time;         // the time at which the initiating packet was formed
    int group_id;
    struct in_addr original_sender; // needed for the unicast communication and downloading/uploading of file
    int type;   // 0 for normal message, 1 for search file, 2 for vote, 3 for create_group notif
}packet;


//group_info related globals
group_info groups[MAXGRPSIZE];
int my_fd, system_fd, uni_fd;               // my_fd is used for all multicast groups, system_fd
int group_index = 0;                // the last index to which groups are filled
int membership[MAXGRPSIZE];         // tells if (group_id)th group has been joined by the user or not
int id_used[MAXGRPSIZE];            // tells us if the id is already in use
int sharing =0;                     // to let the forever loop know that its time to share data
//addr and sending related globals
struct sockaddr_in my_addr;          // own addr for muticast groups
struct sockaddr_in my_sys_addr;     // own addr for create_groups notifs
struct sockaddr_in dst_addr;        // addr of the multicast or single destination
struct sockaddr_in recv_addr;       // addr of multicast group or the sender of PM
struct sockaddr_in uni_addr;       // own addr for checking unicast replies and donwloads
struct sockaddr_in uni_recv_addr;   // just created this to avoid conflict between threads
struct sockaddr_in shr_addr;        //addr of sharing groups
packet* send_grp,* rcv_grp,* recv_indi,* send_indi,* sharing_msg;

//voting related globals
int favor=0, against=0;

//file related globals
const char delim[2] = ",";
char my_filename[MAXFILESYS][MAXFILENAME];
char net_filename[MAXFILENET][MAXFILENAME];
int my_file_index;
int net_file_index;

//mutexes
pthread_mutex_t input = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t group_data = PTHREAD_MUTEX_INITIALIZER;

timer_t* t;
struct itimerspec its;
struct sigevent te;
sigset_t alarm_mask;
    

//func declarations
void populate_packet(packet* pkt, char* msg, int init_time, int group_id, struct in_addr og_sender, int type );
void request_group_info();                  //
void refresh_file_info();                   // updates the my file array of strings
void refresh_group_info();                   // updates the my file array of strings
void display_file_data();                   // used to display the  knwon files in system and netwrok.
int get_min_id();
int find_group(int group_id);
int check_collision(char* ip);
void create_group();
void join_group();
void search_group();
void send_msg();
int search_file(char* filename) ;
void start_vote();
void closing_up(int sig_no);
void* RecvMsg() ;
void handler(int sig, siginfo_t *si, void *uc);
void request_data();
void share_data(int option, struct sockaddr_in* recv_addr, packet* recv_grp);
int file_exists(char* filenmae, int options);

void die(char *s)
{
    perror(s);
    exit(1);
}
void populate_packet(packet* pkt, char* msg, int init_time, int group_id, struct in_addr og_sender, int type )
{
    memset(pkt, 0, sizeof(pkt));
    strcpy(pkt->msg,msg);
    pkt->type = type;
    pkt->original_sender = og_sender;
    pkt->group_id = group_id;
    pkt->init_time = init_time;
    return;
};
void request_group_info()
{
    char buf[MSGBUFSIZE] = "NEED INFO";
    int nbytes = sendto(system_fd, buf,sizeof(buf), 0,(struct sockaddr *)&my_sys_addr, sizeof(my_sys_addr));
    refresh_group_info();
}
void refresh_group_info()                  //rcv broadcast till all the group updation has occured
{
    printf("Getting infornmation about groups created by other users...\n");
    pthread_mutex_lock(&input);
    group_info temp;
    int addrlen;
    while(recvfrom(system_fd, &temp, sizeof(group_info), 0, (struct sockaddr*)&recv_addr, &addrlen) > 0)
    {
        if(id_used[temp.group_id] == 1);
            continue;
        groups[group_index].group_id = temp.group_id;
        strcpy(groups[group_index].ip, temp.ip);
        strcpy(groups[group_index].name, temp.name);
        group_index++;
        id_used[temp.group_id] = 1;
    }
    printf("Finished retrieving the group information on the network\n");
    pthread_mutex_unlock(&input);
}
void refresh_file_info()           
{
    struct dirent *de ;
    DIR *dr = opendir(".");
    if (dr == NULL)
    {
        printf("Could not open current directory\n" );
        return;
    }
    my_file_index= 0;
    while ((de = readdir(dr)) != NULL)
    {
        //printf("%s\n", de->d_name);
        if(strcmp(de->d_name, ".")==0||strcmp(de->d_name, "..")==0)
        {
            continue;
        }
        if(my_file_index < MAXFILESYS)
        {
            strcpy(&(my_filename[my_file_index][0]), de->d_name);
            my_file_index++;
        }   
        if(file_exists(de->d_name, 0)==0&& my_file_index < MAXFILENET)
        {    
            strcpy(&(net_filename[net_file_index][0]), de->d_name);
            net_file_index++;
        }
    }
    closedir(dr);
    return;
}
void display_file_data()
{
    pthread_mutex_lock(&input);
    printf("The followinf files are present on this system \n");
    for(int i=0; i <my_file_index ; i++)
    {
        printf("%s\n", my_filename[i]);
    }

    printf("\n");
    pthread_mutex_unlock(&input);

    pthread_mutex_lock(&input);
    printf("The following files are present on the network (including the files on this system)\n");
    for(int i=0; i <my_file_index ; i++)
    {
        printf("%s\n", net_filename[i]);
    }
    pthread_mutex_unlock(&input);
}
int file_exists(char* filename, int options)
{
    if(options)         //checks if file exists in system records or not
    {
        for(int i=0; i < my_file_index; i++)
        {
            if(strcmp(filename, my_filename[i])==0)
            return 1;             
        }   
        return 0;
    }
    else                //check if file exists in network records or not
    {

        for(int i=0; i < net_file_index; i++)
        {
            if(strcmp(filename, net_filename[i])==0)
            return 1;             
        }   
        return 0;

    }
}
int get_min_id(){
    for(int i=0; i < MAXGRPSIZE; i++)
    {
        if(id_used[i]==0)
        {
            return i;
        }
    }
    return -1;}                                            // reutn the minimum id which can be used to identify a new group

int find_group(int group_id){
    for(int i=0; i < group_index; i ++)
    {
        if(groups[i].group_id == group_id)
        {
            return i;
        }
    }
    return -1;}                     //find index of the group with given group_id

int check_collision(char* ip)              //check if group with this ip already exists
{
    for(int i=0; i < group_index; i++)
    {
        if(strcmp(groups[i].ip, ip)==0)
            return 1;
    }
    return 0;}

void create_group()
{
    refresh_group_info();
    if(group_index == MAXGRPSIZE)       // in case maximum groups have been created
    {
        printf("Maximum number of groups have been created on this LAN\n");
        return;
    }

    int group_id = get_min_id();
    if(group_id == -1)      // in case no group_id is available for the new group
    {
        printf("There is no free group ID available\n");
        return;
    }

    char ip[MAXIPSIZE];
    printf("Please type the multicast IP for the group: ");
    scanf("%s", ip);
    if(check_collision(ip)) // in case a group with this ip already exists
        {
            printf("A group with such ip already exists.\nPlease join that group instaed of creating one or use another IP for group formation\n");
            return;
        }
    char name[GRPNAMESIZE];
    printf("Please type the group name (NO white spaces allowed in the name): ");
    scanf("%s", name);
    // Adding the membership of that group
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(ip);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);

    if (setsockopt(my_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
        die("add mem");

    // updating the group info array for future references.
    groups[group_index].group_id = get_min_id();
    strcpy(groups[group_index].ip, ip);
    strcpy(groups[group_index].name, name);

    group_info temp;
    temp.group_id = get_min_id();
    strcpy(temp.ip ,ip);
    strcpy(temp.name , name);

    group_index++;
    membership[group_id] = 1;
    id_used[group_id] = 1;
    printf("Group successfully created\n");
    //broadcast this info
    int nbytes = sendto(system_fd, &temp, sizeof(group_info), 0,(struct sockaddr *)&my_sys_addr, sizeof(my_sys_addr));
}
void join_group()
{

    refresh_group_info();

    int group_id;
    printf("Please type the group ID of the group you want to join: ");
    scanf("%d", &group_id);
    if(membership[group_id])
    {
        printf("You are already a part of the group you desire to Join\n");\
        return;
    }
    int index  = find_group(group_id);
    if(index ==-1)
    {
        printf("No group with such ID exists known to this user\n");
        return;
    }

    group_info temp = groups[index];
    char ip[MAXIPSIZE];
    strcpy(ip, temp.ip);

    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(ip);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(my_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
        die("add mem");
    printf("Group successfully Joined\n");

    membership[group_id] = 1;
}
void search_group()
{
    refresh_group_info();
    //problem if one joins after the group fomration then the broadcast was never intended for this guy;
    pthread_mutex_lock(&input);
    if(group_index==0)
    {
        printf("There are no groups on the LAN\n");
        pthread_mutex_unlock(&input);
        return;
    }
    int choice;
    printf("Type the group ID of the group whose Data you want, and -1 if you want for all known groups\n");
    scanf("%d", &choice);

    if(choice==-1)
    {
        printf("Here are all the groups in the following format\n");
        printf("Name\t\t Group ID\t IP\t\t port\n\n\n");
        for(int i=0; i < group_index; i++)
        {
            printf("%s\t\t %d\t %s\t\t %d\n", groups[i].name, groups[i].group_id, groups[i].ip, PORT1);
        }
        pthread_mutex_unlock(&input);
        return;
    }
    else
    {
        int index = find_group(choice);
        if(index==-1)
        {
            printf("No group with such Group ID exists\n");    
            pthread_mutex_unlock(&input);
            return;
        }
        printf("Here is the group whose information was desired\n");
        printf("Name\t Group ID\t IP\t port\n");
        printf("%s\t %d\t %s\t %d\n", groups[index].name, groups[index].group_id, groups[index].ip, PORT1);
        pthread_mutex_unlock(&input);
        return;   
    }
}
void send_msg()
{
    int group_id;
    char* msg = malloc(sizeof(char)*MSGBUFSIZE);

    pthread_mutex_lock(&input);
    if (sigprocmask(SIG_BLOCK, &alarm_mask, NULL) == -1)
                    die("Couldn't block SIGALRM\n");
    printf("Please type the group ID of the group you want to send a message to \n");
    scanf("%d", &group_id);getchar();
    int index  = find_group(group_id);
    if(index ==-1)
        {
        printf("No group with such ID exists known to this user\n");
        pthread_mutex_unlock(&input);
        return;
    }
    if(membership[group_id]==0)
    {
        printf("You are not a member of such a Group ID\n");
        pthread_mutex_unlock(&input);
        return;
    }
    printf("Please type the message you want to send (< 1000 characters) \n");
    scanf("%[^\n]", msg);getchar();
    if (sigprocmask(SIG_UNBLOCK, &alarm_mask, NULL) == -1)
                    die("Couldn't unblock SIGALRM\n");
    
    pthread_mutex_unlock(&input);

    struct timeval struct_time;
    gettimeofday(&struct_time, NULL);

    memset(&dst_addr, 0, sizeof(dst_addr));
    dst_addr.sin_family = AF_INET;
    dst_addr.sin_addr.s_addr = inet_addr(groups[index].ip);
    dst_addr.sin_port = htons(PORT1);

    populate_packet(send_grp, msg, struct_time.tv_sec, group_id,my_addr.sin_addr, 0);
    int nbytes = sendto(my_fd, send_grp, sizeof(packet), 0, (const struct sockaddr *)&dst_addr, sizeof(dst_addr));
    free(msg);
}
int search_file(char* filename)             
{
    refresh_file_info();
    int found = file_exists(filename, 1);
    if(found)
    {
        pthread_mutex_lock(&input);
        printf("The file was found in this system only.\n");
        pthread_mutex_unlock(&input);
        return 1;
    }
    pthread_mutex_lock(&input);
    printf("The file was not found in this system.\nTrying to find it on the network\n");
    struct timeval struct_time;
    gettimeofday(&struct_time, NULL);

    //we used the uni_addr because that address is to be used by the recivers to send the file, if available
    populate_packet(send_grp, filename, struct_time.tv_sec,0,uni_addr.sin_addr, 1);
    int init_time = struct_time.tv_sec;
    for(int i=0; i < MAXGRPSIZE; i++)
    {
        if(membership[i]==1)
        {
            int index  = find_group(i);
            struct timeval struct_time;
            gettimeofday(&struct_time, NULL);

            memset(&dst_addr, 0, sizeof(dst_addr));
            dst_addr.sin_family = AF_INET;
            dst_addr.sin_addr.s_addr = inet_addr(groups[index].ip);
            dst_addr.sin_port = htons(PORT1);

            send_grp->group_id = groups[i].group_id;
            int nbytes = sendto(my_fd, send_grp, sizeof(packet), 0, (const struct sockaddr *)&dst_addr, sizeof(dst_addr));
        }
    }
    if (sigprocmask(SIG_BLOCK, &alarm_mask, NULL) == -1)
                    die("Couldn't block SIGALRM\n");
   
    printf("Waiting for 60 seconds to receive reply of other users for the queried file\n");
    pthread_mutex_unlock(&input);

    int addrlen = sizeof(uni_recv_addr);
    found=0;
    fd_set rset;
    FD_ZERO(&rset);
    while(1)
    {
        FD_SET(uni_fd, &rset);
        struct timeval timeout;
        timeout.tv_sec = 60;
        int ret = select(uni_fd + 1, &rset, NULL, NULL, &timeout);
        if(ret==0)
        {
            found = 0;
            break;
        }
        if(FD_ISSET(uni_fd, &rset))
        {   
            memset(recv_indi, 0 , sizeof(packet));
            if(recvfrom(uni_fd, recv_indi, sizeof(packet), 0, (struct sockaddr*)&uni_recv_addr, &addrlen) <= 0)
            continue;
            if(strcmp(recv_indi->msg, filename)== 0 && recv_indi->type==1)
            {
                int out_fd = open(filename,O_RDWR|O_CREAT,0777);
                char buf[1000];
                pthread_mutex_lock(&input);
                printf("Downloading the file, Please wait...\n");
                int ret;
                found = 1;
                errno = 0;
                while((ret = recvfrom(uni_fd, buf, 1000 *sizeof(char), 0, (struct sockaddr*)&uni_recv_addr, &addrlen)) > 0)
                {
                    errno = 0;
                    write(out_fd, buf, ret);
                }
                pthread_mutex_unlock(&input);
                break;
            }
            else
            {
                continue;
            }       
        }
        
    }
    if(found)
    {
        printf("The file was found on the network and downloaded\n");
        
   
    }
    else
    {
        printf("The file was not found in this network at all.\n");
    }
    if (sigprocmask(SIG_UNBLOCK, &alarm_mask, NULL) == -1)
            die("Couldn't unblock SIGALRM\n");
    return 0;
}
void start_vote()
{
    int group_id;
    char* msg = malloc(sizeof(char)*MSGBUFSIZE);
    pthread_mutex_lock(&input);
    if (sigprocmask(SIG_BLOCK, &alarm_mask, NULL) == -1)
                    die("Couldn't block SIGALRM\n");
    printf("Please type the group ID of the group you want to initiate a vote in: \n");
    scanf("%d", &group_id);getchar();
    printf("Please type a message about the vote you want to start (< 256 characters), end it with the ENTER key: \n");
    scanf("%[^\n]", msg);getchar();
    
    if (sigprocmask(SIG_UNBLOCK, &alarm_mask, NULL) == -1)
        die("Couldn't unblock SIGALRM\n");
               
    int index  = find_group(group_id);
    if(index ==-1)
    {
        printf("No group with such ID exists known to this user\n");
        pthread_mutex_unlock(&input);
        return;
    }

    if(membership[group_id]==0)
    {
        printf("You are not a member of such a group_id \n");
        pthread_mutex_unlock(&input);
        return; 
    }
    pthread_mutex_unlock(&input);

    memset(&dst_addr, 0, sizeof(dst_addr));
    dst_addr.sin_family = AF_INET;
    dst_addr.sin_addr.s_addr = inet_addr(groups[index].ip);
    dst_addr.sin_port = htons(PORT1);
    struct timeval struct_time;
    gettimeofday(&struct_time, NULL);
    favor = 0; against = 0;
    populate_packet(send_grp, msg, struct_time.tv_sec, group_id,my_addr.sin_addr, 2);
    int nbytes = sendto(my_fd, send_grp, sizeof(packet), 0, (const struct sockaddr *)&dst_addr, sizeof(dst_addr));
    if (sigprocmask(SIG_BLOCK, &alarm_mask, NULL) == -1)
                    die("Couldn't block SIGALRM\n");
  
    pthread_mutex_lock(&input);
    printf("Please wait for 30 seconds while we wait for others to vote for/against the motion\n");
    pthread_mutex_unlock(&input);
    sleep(30);  // the reciving thread would update the favor and against global variables in the meanwhile
    pthread_mutex_lock(&input);
    sprintf(msg, "There were %d votes in favor and %d votes in against\n", favor, against);
    gettimeofday(&struct_time, NULL);
    populate_packet(send_grp, msg, struct_time.tv_sec, group_id,my_addr.sin_addr, 0);
    favor = 0; against = 0;
    nbytes = sendto(my_fd, send_grp, sizeof(packet), 0, (const struct sockaddr *)&dst_addr, sizeof(dst_addr));
    
   
   if (sigprocmask(SIG_UNBLOCK, &alarm_mask, NULL) == -1)
        die("Couldn't unblock SIGALRM\n");
     
    pthread_mutex_unlock(&input);

    free(msg);
}
void* RecvMsg()        //remaining the upload thread part
{
    packet* rcv_grp = (packet* )malloc(sizeof(packet));
    int addrlen= sizeof(recv_addr);

    while(1)
    {
        int nbytes = recvfrom(my_fd, rcv_grp, sizeof(packet), 0, (struct sockaddr*)&recv_addr, &addrlen);
        if(nbytes < 1)
        {
            continue;
        }
        int type  = rcv_grp->type;
        switch(type)
        {
            case 0: // a normal group message to be displayed
            {
                int group_id  = rcv_grp->group_id;
                struct in_addr sender = rcv_grp->original_sender;
                char buf[MSGBUFSIZE];
                strcpy(buf, rcv_grp->msg);
                int index = find_group(group_id);
                char * user_IP = malloc(sizeof(char)*MAXIPSIZE) ;
                inet_ntop(AF_INET, &(rcv_grp->original_sender),user_IP, MAXIPSIZE);
                printf("\nReceived a message from user %s in group %s: \n",user_IP, groups[index].name);
                printf("%s\n\n",buf);    
                break;
            }
            case 1:     //search request
            {
                refresh_file_info();
                int found  = file_exists(rcv_grp->msg, 1);
                if(found==0)
                {

                    recv_addr.sin_addr = rcv_grp->original_sender;
                    recv_addr.sin_port =  htons(PORT3);
                    populate_packet(send_indi, rcv_grp->msg, rcv_grp->init_time, -1 ,uni_addr.sin_addr, 0);

                    // send msg to query giver that file was not found
                    int nbytes = sendto(uni_fd, send_indi, sizeof(packet), 0, (const struct sockaddr *)&recv_addr, sizeof(recv_addr));    /* code */
                }
                else
                {
                    recv_addr.sin_addr = rcv_grp->original_sender;
                    recv_addr.sin_port =  htons(PORT3);
                    populate_packet(send_indi, rcv_grp->msg, rcv_grp->init_time, -1 ,uni_addr.sin_addr, 1);
                    // send msg to query giver that file was found
                    int nbytes = sendto(uni_fd, send_indi, sizeof(packet), 0, (const struct sockaddr *)&recv_addr, sizeof(recv_addr));

                    int in_fd = open (rcv_grp->msg, O_RDONLY);
                    if(in_fd < 0)
                    {
                        printf("A file which was requested couldn't be sent because the file couldn't be opened\n");
                    }
                    else
                    {
                        char buf[MSGBUFSIZE];
                        int ret, filebyte=0;
                        while((ret = read(in_fd,buf,1000)) > 0)
                            {
                               nbytes = sendto(uni_fd,buf, ret , 0, (const struct sockaddr *)&recv_addr, sizeof(recv_addr));
                               //printf("this much was sent in a chunk %d\n", nbytes );
                               filebyte+=ret;
                            }
                            if(errno == EAGAIN)
                            {
                                printf("The file was found on the system and requested but could not be read properly\n");
                            }
                        //printf("This is the sent data size: %d\n", filebyte);
                    }
                }
                break;
            }
            case 2:     //vote request
            {
                struct timeval struct_time; 
                int group_id  = rcv_grp->group_id;
                struct in_addr sender = rcv_grp->original_sender;
                char buf[MSGBUFSIZE];
                strcpy(buf, rcv_grp->msg);
                int index = find_group(group_id);
                char * user_IP = malloc(sizeof(char)*MAXIPSIZE) ;
                inet_ntop(AF_INET, &(rcv_grp->original_sender),user_IP, MAXIPSIZE);
                pthread_mutex_lock(&input);
                if (sigprocmask(SIG_BLOCK, &alarm_mask, NULL) == -1)
                    die("Couldn't block SIGALRM\n");
               
                printf("User: %s from Group: %s initiated a poll on the following matter: \n",user_IP , groups[index].name);
                printf("%s\n\n" ,buf);
                printf("Please type y if you are in favor and n if you are against within 30 seconds.\n");
                scanf("%s" ,buf);
                gettimeofday(&struct_time, NULL);
                if(struct_time.tv_sec - rcv_grp->init_time > 30)
                {
                    printf("You took more than 30 seconds, your vote would not be considered\n");
                }
                if (sigprocmask(SIG_UNBLOCK, &alarm_mask, NULL) == -1)
                    die("Couldn't unblock SIGALRM\n");
                
                if (strcmp(buf, "y")==0||strcmp(buf, "n")==0)
                {
                    pthread_mutex_unlock(&input);
                    int index = find_group(group_id);
                    recv_addr.sin_addr.s_addr = inet_addr(groups[index].ip);
                    populate_packet(rcv_grp, buf, rcv_grp->init_time, group_id ,sender, 3);
                    int nbytes = sendto(my_fd, rcv_grp, sizeof(packet), 0, (const struct sockaddr *)&recv_addr, sizeof(recv_addr));
                     /* code */
                }
                else
                {
                    printf("You typed %s which is not a valid answer and thus your vote has been discarded\n", buf);
                    printf("You can ask the group to initiate the poll again after 30 seconds if you want\n");
                    pthread_mutex_unlock(&input);

                }
                    break;
            }
            case 3: //vote response
            {
                struct timeval struct_time;
                gettimeofday(&struct_time, NULL);
                int now = struct_time.tv_sec;
                
                if(((now - rcv_grp->init_time) < 30) && rcv_grp->original_sender.s_addr == my_addr.sin_addr.s_addr)
                {
                    if (strcmp(rcv_grp->msg, "y")==0)
                    {
                        favor++;
                    }
                    else if(strcmp(rcv_grp->msg, "n")==0)
                    {
                        against++;
                    }
                }
                break;
            }
            case 4: // request data msg
            {
                if((rcv_grp->original_sender).s_addr==my_addr.sin_addr.s_addr)
                   break;  
                //printf("Someone has Requested Data\n");
                share_data(1, &recv_addr ,rcv_grp);
                break;
            }
            case 5:     //msg with info to change in the shared database of files
            {
                if((rcv_grp->original_sender).s_addr==my_addr.sin_addr.s_addr)
                    break;
                printf("Some updated Data Received\n");
                char buf[MSGBUFSIZE];
                strcpy(buf, rcv_grp->msg);
                char* token = malloc(sizeof(char)*MAXFILENAME) ;
                token = strtok(buf,delim);
                while( token != NULL && net_file_index < MAXFILENET )
                {
                    if(file_exists(token, 1)==1)
                    {
                       // printf("%s\n",token );
                        token = strtok(NULL, delim);continue;
                    }
                    strcpy(net_filename[net_file_index],token);
                    net_file_index++;
                    token = strtok(NULL, delim);
                }
                free(token);
                break;
            }
            default:
            {
                pthread_mutex_lock(&input);
                printf("A strange packet was recived which is not of the defined types\n");
                pthread_mutex_unlock(&input);
            }
        }
    }
}
void handler(int sig, siginfo_t *si, void *uc)
{
    sharing = 1;
    if(timer_settime(*t, 0, &its, NULL) == -1)
            die("timer");
}
void request_data()
{
    int group_id;
    char msg[MSGBUFSIZE]="SEND FILE DATA";
    for(int i=0; i < MAXGRPSIZE; i++)
    {
        if(membership[i]==1)
        {
            int index  = find_group(i);
            struct timeval struct_time;
            gettimeofday(&struct_time, NULL);

            memset(&dst_addr, 0, sizeof(dst_addr));
            dst_addr.sin_family = AF_INET;
            dst_addr.sin_addr.s_addr = inet_addr(groups[index].ip);
            dst_addr.sin_port = htons(PORT1);
            populate_packet(send_grp, msg, struct_time.tv_sec, i ,my_addr.sin_addr, 4);
            int nbytes = sendto(my_fd, send_grp, sizeof(packet), 0, (const struct sockaddr *)&dst_addr, sizeof(dst_addr));
        }
    }
    printf("All requests for data have been sent\n");
    // would recv the updates through the recv msg thread which would update the global variables of file_available
}
void share_data(int option, struct sockaddr_in* recv_addr, packet* recv_grp)
{
    refresh_file_info();
    char msg[MSGBUFSIZE];
    sharing_msg = malloc(sizeof(packet));
    if(net_file_index  < 1)
    {
        sharing = 0;
        free(sharing_msg);
        return;
    }
    strcat(msg, net_filename[0]);
    for(int i = 1; i < net_file_index; i++)
    {
        strcat(msg, delim);
        strcat(msg, net_filename[i]);

    }
    struct timeval struct_time;

    if(option)          //to be shared with the requestor only
    {
        int group_id = recv_grp->group_id;
        int index = find_group(group_id);
        memset(&shr_addr, 0, sizeof(shr_addr));
        shr_addr.sin_family=AF_INET;
        shr_addr.sin_addr.s_addr = inet_addr(groups[index].ip);
        shr_addr.sin_port = htons(PORT1);
        gettimeofday(&struct_time, NULL);
        populate_packet(sharing_msg,msg,struct_time.tv_sec, group_id ,my_addr.sin_addr,5);
        int nbytes = sendto(my_fd, sharing_msg, sizeof(packet), 0, (const struct sockaddr *)&shr_addr, sizeof(shr_addr));
        free(sharing_msg);
        return;
    }
    int group_id;
    for(int i=0; i < MAXGRPSIZE; i++)       // to be shared on all the groups this user is a member of
    {
        if(membership[i]==1)
        {
            int index  = find_group(i);
            gettimeofday(&struct_time, NULL);
            memset(&shr_addr, 0, sizeof(shr_addr));
            shr_addr.sin_family = AF_INET;
            shr_addr.sin_addr.s_addr = inet_addr(groups[index].ip);
            shr_addr.sin_port = htons(PORT1);

            // how to convert this msg into a string of string to string which is to be sent, concat?
            populate_packet(sharing_msg,msg,struct_time.tv_sec, groups[index].group_id ,my_addr.sin_addr,5);
            int nbytes = sendto(my_fd, sharing_msg, sizeof(packet), 0, (const struct sockaddr *)&shr_addr, sizeof(shr_addr));
        }
    }
    printf("Sharing Complete..\n");
    free(sharing_msg);
}

int main(int argc, char *argv[])
{
    refresh_file_info();
    my_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (my_fd < 0)
    {
        perror("socket");
        return 1;
    }
    system_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (system_fd < 0)
    {
        perror("socket");
        return 1;
    }
    uni_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (my_fd < 0)
    {
        perror("socket");
        return 1;
    }
    struct timeval dum;
    dum.tv_sec = 2;
    dum.tv_usec = 0;
    u_int yes = 1;
    if (setsockopt(system_fd, SOL_SOCKET, SO_BROADCAST, (char*) &yes, sizeof(yes)) < 0)
        die("Setting A Broadcast Failed");
    if (setsockopt(system_fd, SOL_SOCKET, SO_RCVTIMEO, &dum , sizeof(struct timeval)) < 0)
        die("Setting A Non-Blocking Receive Failed");
    
    if (setsockopt(uni_fd, SOL_SOCKET,SO_RCVTIMEO, &dum , sizeof(struct timeval)) < 0)
        die("Setting A Unicast failed");

    // set up my address
    memset(&my_addr, 0, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_addr.s_addr = htonl(INADDR_ANY); // differs from sender
    my_addr.sin_port = htons(PORT1);
    if (bind(my_fd, (struct sockaddr*) &my_addr, sizeof(my_addr)) < 0)
        die("bind Multicast");


    /*Use this addr to get the broadcast messages for create_group changes*/
    memset(&my_sys_addr, 0, sizeof(my_sys_addr));
    my_sys_addr.sin_family = AF_INET;
    my_sys_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    my_sys_addr.sin_port = htons(PORT2);
    if (bind(system_fd, (struct sockaddr*) &my_sys_addr, sizeof(my_sys_addr)) < 0)
        die("bind Broadcast");

    memset(&uni_addr, 0, sizeof(uni_addr));
    uni_addr.sin_family = AF_INET;
    uni_addr.sin_addr.s_addr = htonl(INADDR_ANY); // differs from sender
    uni_addr.sin_port = htons(PORT3);
    if (bind(uni_fd, (struct sockaddr*) &uni_addr, sizeof(uni_addr)) < 0)
        die("bind Unicast");

    int ret;
    send_grp = (packet*)malloc(sizeof(packet));
    send_indi = (packet*)malloc(sizeof(packet));
    recv_indi = (packet*)malloc(sizeof(packet));

    pthread_t p_recv;
    pthread_attr_t attr;
    pthread_attr_init (&attr);
    pthread_create (&p_recv, &attr, RecvMsg , NULL);
    // remember to convert these into masks

    struct sigaction sa;
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGALRM, &sa, NULL) == -1)
        die("signal handler");
    
    t = (timer_t*)malloc(sizeof(timer_t));
    
    te.sigev_notify = SIGEV_SIGNAL;
    te.sigev_signo = SIGALRM;
    te.sigev_value.sival_ptr = t;
    
    timer_create(CLOCK_REALTIME, &te, t);
    
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 0;
    its.it_value.tv_nsec = 0;
    its.it_value.tv_sec = 60;
    if ((sigemptyset(&alarm_mask) == -1) || (sigaddset(&alarm_mask, SIGALRM) == -1))
        die("Failed to initialize the signal mask");
   
    if(timer_settime(*t, 0, &its, NULL) == -1)
        die("timer");
    
    printf("Press Ctrl + C to shut down the application\n");
    request_group_info();
    pthread_mutex_unlock(&input);
    while(1)
    {
        if(sharing==1)
        {
            pthread_mutex_lock(&input);
            printf("Sharing data with network peers.. Please Wait\n");
            share_data(0, NULL, NULL);
            pthread_mutex_unlock(&input);
            sharing = 0;
        }
        int options = 0;
        pthread_mutex_lock(&input);

        printf("************************************\n");
        printf("Please choose one of the options\n");
        printf("\t1) Create A Group\n");
        printf("\t2) Join A Group\n");
        printf("\t3) Search for a Group\n");
        printf("\t4) Send a message to a Group\n");
        printf("\t5) Search for a File\n");
        printf("\t6) Initiate a vote in a Group\n");
        printf("\t7) Request data from others\n");
        printf("\t8) Display all the file information on system and known network\n");
        printf("************************************\n");
        char msg[MSGBUFSIZE];
        int addrlen =0;
        recvfrom(system_fd, msg , MSGBUFSIZE , 0, (struct sockaddr*)&recv_addr, &addrlen);
        if(strcmp(msg, "NEED INFO" )==0)
        {
            sendto(system_fd, &groups, group_index*sizeof(group_info), 0,(const struct sockaddr *)&my_sys_addr, sizeof(my_sys_addr));
            memset(&msg, 0, MSGBUFSIZE);  
        }       
        scanf("%d",&options);
        pthread_mutex_unlock(&input);

        switch(options)
        {
            case 1:
            {
                create_group();
                break;
            }
            case 2:
            {
                join_group();
                break;
            }
            case 3:
            {
                search_group();
                break;
            }
            case 4:
            {
                send_msg();
                break;
            }
            case 5:
            {
                char* filename = malloc(sizeof(char)* MSGBUFSIZE);
                pthread_mutex_lock(&input);
                if (sigprocmask(SIG_BLOCK, &alarm_mask, NULL) == -1)
                    die("Couldn't block SIGALRM\n");
   
                printf("Please type the name of the file you want to search: ");
                scanf("%s", filename);
                if (sigprocmask(SIG_UNBLOCK, &alarm_mask, NULL) == -1)
                    die("Couldn't unblock SIGALRM\n");
                pthread_mutex_unlock(&input);
                search_file(filename);
                free(filename);
                break;
            }
            case 6:
            {
                start_vote();
                break;
            }
            case 7:
            {
                request_data();
                break;
            }
            case 8:
            {
                display_file_data();
                break;
            }
            default:
            {
                if(sharing)
                {
                    break;
                }
                printf("Please select a valid option and type a number between 1-8 corresponding to your choice\n");
            }

        }
    }
    //join the recv msg handler somehow

    return 0;

}