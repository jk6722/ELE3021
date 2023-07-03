// Process manager

#include "types.h"
#include "user.h"
#include "fcntl.h"
#define MAXARGS 10

struct cmd {
  char *type;
  char *argv[MAXARGS];
};

void
panic(char *s)
{
  printf(2, "%s\n", s);
  exit();
}

int
fork1(void)
{
  int pid;

  pid = fork();
  if(pid == -1)
    panic("fork");
  return pid;
}

void
runcmd(struct cmd* cmd)
{
  if(cmd == 0){
    exit();
  }
  if(!strcmp(cmd->type, "list")) {
    printlist();
  }
  else if(!strcmp(cmd->type, "kill")) {
    kill(atoi(cmd->argv[0]));
  }
  else if(!strcmp(cmd->type, "execute")) {
    int stacksize = atoi(cmd->argv[1]);
    cmd->argv[1] = 0;
    if(fork1() == 0)
      exec2(cmd->argv[0], cmd->argv, stacksize);
    exit();
  }
  else if(!strcmp(cmd->type, "memlim")) {
    if(setmemorylimit(atoi(cmd->argv[0]), atoi(cmd->argv[1])) < 0)
      printf(1, "memlim fail\n");
  }
  else panic("invalid command");
  exit();
}

void
strcopy(char *buf1, char *buf2)
{
  for(int i = 0; i <= strlen(buf2); i++){
    buf1[i] = buf2[i];
    if(buf2[i] == '\0') break;
  }
}

void
resetbuff(char* buf, int size)
{
  for(int i = 0; i <= size; i++)
    buf[i] = 0;
}

struct cmd*
parsecmd(char* buf) {
  struct cmd* cmd;
  char temp[100]; // buff for parsing
  memset(temp, 0, sizeof(temp));
  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = malloc(100);
  for(int i = 0; i<MAXARGS; i++){
    cmd->argv[i] = malloc(100);
  }
  int temp_idx = 0;
  int argc = -1;

  for(int i = 0; i<100; i++)
    if(buf[i] == '\n'){
      buf[i] = '\0';
      break;
    }

  for(int i = 0; i<100; i++){
    temp[temp_idx] = buf[i];
    if(temp[temp_idx] == ' ' || temp[temp_idx] == '\0'){
      temp[temp_idx] = '\0';
      if(argc == -1){
        strcopy(cmd->type, temp);
        argc++;
      }
      else strcopy(cmd->argv[argc++], temp);
      resetbuff(temp, strlen(temp));
      if(argc >= MAXARGS){
        panic("too many args");
        return 0;
      }
      if(buf[i] == '\0') break;
      temp_idx = 0;
      continue;
    }
    temp_idx++;
  }
  return cmd;
}

int
getcmd(char *buf, int nbuf)
{
  printf(2, "=> ");
  memset(buf, 0, nbuf);
  gets(buf, nbuf);
  if(buf[0] == 0) // EOF
    return -1;
  return 0;
}

int
main(void)
{
  static char buf[100];
  int fd;
  // Ensure that three file descriptors are open.
  while((fd = open("console", O_RDWR)) >= 0){
    if(fd >= 3){
      close(fd);
      break;
    }
  }
  // Read and run input commands.
  while(getcmd(buf, sizeof(buf)) >= 0){
    struct cmd* cmd = parsecmd(buf);
    if(!strcmp(cmd->type, "exit")) break;
    if(fork1() == 0){
      runcmd(cmd);
    }
    wait();
  }
  printf(1, "exit pmanager\n");
  exit();
}
