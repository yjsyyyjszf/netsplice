#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <errno.h>

#include <iostream>
#include <iomanip>
#include <string_view>
#include <vector>

using namespace std;

#define PORT_NUMBER 5001
static std::string filesizeReadableArray[] = {"b", "KiB", "MiB", "GiB", "TiB"};

// C++ with it's crappy string manipulation tools... This is merely to get 2 decimal precision in the filesize.
template <typename T>
std::string to_string(const T val, const int prec)
{
  std::ostringstream os;
  os << std::fixed << std::setprecision(prec) << val;
  return os.str();
}

std::pair<int, double> filesize(const uint64_t size)
{
  double newSize = size;
  int index = 0;
  while (newSize > 1024)
  {
    newSize /= 1024;
    index++;
  }

  return std::make_pair(index, newSize);
}

std::string filesizeString(const uint64_t size)
{
  auto [index, newSize] = filesize(size);
  return to_string(newSize, 2) + " " + filesizeReadableArray[index];
}

std::string_view filename(const std::string_view &fullFileName)
{
  return fullFileName.substr(fullFileName.find_last_of('/') + 1);
}

void setSocketOptions(int connection)
{
  /*
  int flag = 1;
  if (setsockopt(connection, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) == -1)
  {
    perror("setsockopt(TCP_NODELAY=1)");
  }

  int corkFlag = 1;
  if (setsockopt(connection, IPPROTO_TCP, TCP_CORK, &corkFlag, sizeof(corkFlag)) == -1)
  {
    perror("setsockopt(TCP_CORK=1)");
  }

  int quickAckFlag = 1;
  if (setsockopt(connection, IPPROTO_TCP, TCP_QUICKACK, (char *) &quickAckFlag, sizeof(int) == -1))
  {
    perror("setsockopt(TCP_QUICKACK=1)");
    std::cout << strerror(errno) << "\n";
  }
  */
}

//---------------------------------------------------------------------------
#define BUFFER_SIZE (4 * 1024)
uint64_t sendfile_rw(int fd_dst, int fd_src, uint64_t n)
{
  char *buffer[BUFFER_SIZE];
  uint64_t bytes_left = n;

  while (bytes_left > 0)
  {
    uint64_t block_size = (bytes_left < BUFFER_SIZE) ? bytes_left : BUFFER_SIZE;

    if (read(fd_src, buffer, block_size) < 0)
    {
      perror("error while reading");
      break;
    }

    if (write(fd_dst, buffer, block_size) < 0)
    {
      perror("error while writing");
      break;
    }

    bytes_left -= block_size;
  }

  return n - bytes_left;
}

//---------------------------------------------------------------------------
uint64_t sendfile_sendfile(int fd_dst, int fd_src, uint64_t n)
{
  uint64_t bytes_left = n;
  off_t offset = 0;

  while (bytes_left > 0)
  {
    uint64_t block_size = sendfile(fd_dst, fd_src, &offset, bytes_left);
    bytes_left -= block_size;
  }

  return n - bytes_left;
}

//---------------------------------------------------------------------------
#define MAX_SPLICE_SIZE (16 * 1024)
uint64_t sendfile_splice(int fd_dst, int fd_src, uint64_t n)
{
  int pfd[2];
  uint64_t bytes_left = n;
  // loff_t off1;
  // loff_t off2;

  // Create pipe
  if (pipe(pfd) < 0)
  {
    perror("pipe failed");
    return 0;
  }

  while (bytes_left > 0)
  {
    uint64_t block_size = (bytes_left < MAX_SPLICE_SIZE) ? bytes_left : MAX_SPLICE_SIZE;

    // Splice to fd_src -> pipe
    uint64_t splice_size = splice(fd_src, NULL, pfd[1], NULL, block_size, SPLICE_F_MOVE);

    if (splice_size == 0)
    {
      break;
    }

    // Splice pipe -> fd_dst
    splice(pfd[0], NULL, fd_dst, NULL, splice_size, SPLICE_F_MOVE);

    bytes_left -= splice_size;
  }

  return n - bytes_left;
}

//---------------------------------------------------------------------------
int client_start(const char *hostname)
{
  int sockfd;
  struct sockaddr_in serv_addr;
  struct hostent *server;

  sockfd = socket(AF_INET, SOCK_STREAM, 0);

  if (sockfd < 0)
  {
    perror("CClient::run: ERROR opening socket");
    return -1;
  }

  server = gethostbyname(hostname);

  if (server == NULL)
  {
    cout << "CClient::run: ERROR, no such host" << endl;
    return -1;
  }

  bzero((char *)&serv_addr, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  bcopy((char *)server->h_addr, (char *)&serv_addr.sin_addr.s_addr, server->h_length);
  serv_addr.sin_port = htons(PORT_NUMBER);

  if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
  {
    perror("CClient::run: ERROR connecting");
    return -1;
  }

  setSocketOptions(sockfd);

  return sockfd;
}

//---------------------------------------------------------------------------
int server_start()
{
  int sockfd, newsockfd;
  socklen_t clilen;
  struct sockaddr_in serv_addr, cli_addr;

  sockfd = socket(AF_INET, SOCK_STREAM, 0);

  if (sockfd < 0)
  {
    perror("ERROR opening socket");
    return -1;
  }

  bzero((char *)&serv_addr, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = INADDR_ANY;
  serv_addr.sin_port = htons(PORT_NUMBER);

  if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
  {
    perror("ERROR on binding");
    return -1;
  }

  cout << "Waiting for clients..." << endl;

  ::listen(sockfd, 5);
  clilen = sizeof(cli_addr);
  newsockfd = ::accept(sockfd, (struct sockaddr *)&cli_addr, &clilen);

  if (newsockfd < 0)
  {
    perror("ERROR on accept");
    return -1;
  }

  setSocketOptions(newsockfd);

  cout << "Client connected" << endl;

  return newsockfd;
}

//---------------------------------------------------------------------------
void print_usage()
{
  cout << "Usage: netsplice <mode> receive [fileout]" << endl;
  cout << "       netsplice <mode> send localhost filein" << endl;
  cout << "       netsplice <mode> copy filein fileout" << endl;
  cout << endl;
  cout << "       <mode>:" << endl;
  cout << "         rw" << endl;
  cout << "         sendfile" << endl;
  cout << "         splice" << endl;
}

//---------------------------------------------------------------------------
int main(int argc, char *argv[])
{
  int fd_filein;
  int fd_fileout;
  struct stat sb;
  uint64_t filesize;
  uint64_t transferred;

  cout << "netsplice - simple application for transferring files" << endl;

  if (argc < 3)
  {
    print_usage();
    return -1;
  }

  string sMode = argv[1];

  if ((sMode != "rw") && (sMode != "sendfile") && (sMode != "splice"))
  {
    cout << "invalid transfer <mode> selected, choose rw, sendfile or splice"
         << endl;

    print_usage();
    return -1;
  }

  string sAction = argv[2];
  if (sAction == "receive")
  {
    if (argc < 3)
    {
      print_usage();
      return -1;
    }

    fd_filein = server_start();

    if (fd_filein < 0)
    {
      cout << "Unable start socket server" << endl;
      return -1;
    }

    // Get the filesize from the client
    read(fd_filein, &filesize, sizeof(filesize));

    // Get the filename length
    uint64_t filenameLength = 0;
    read(fd_filein, &filenameLength, sizeof(filenameLength));

    // Pre-allocate a string of the filename length. Will be filled in the read command.
    std::vector<char> nameBuffer(filenameLength);
    read(fd_filein, &nameBuffer[0], filenameLength);
    std::string_view name(&nameBuffer[0], nameBuffer.size());

    if (argc >= 4)
    {
      name = std::string_view(argv[3]);
      std::cout << "Using filename provided in command argument: " << name;
    }
    else
    {
      std::cout << "No filename provided in command argument, using the received filename: " << name;
    }

    std::cout << "\n";

    fd_fileout = open(name.data(), O_WRONLY | O_CREAT, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);

    if (fd_fileout < 0)
    {
      perror("Unable to open output file");
      return -1;
    }
  }
  else if (sAction == "send")
  {
    if (argc != 5)
    {
      print_usage();
      return -1;
    }

    fd_filein = open(argv[4], O_RDONLY);

    if (fd_filein < 0)
    {
      perror("Unable to open input file");
      return -1;
    }

    fstat(fd_filein, &sb);
    filesize = sb.st_size;

    cout << "filesize: " << filesize << endl;

    fd_fileout = client_start(argv[3]);

    if (fd_fileout < 0)
    {
      cout << "Unable start socket client" << endl;
      return -1;
    }

    // Tell the server what we are about to send
    write(fd_fileout, &filesize, sizeof(filesize));

    std::string_view name(argv[4]);
    uint64_t filenameLength = name.length();
    std::cout << "filename: " << name << " length: " << filenameLength << "\n";

    std::cout << "filesize obj bytes: " << sizeof(filesize) << "\n";
    std::cout << "filename obj bytes: " << sizeof(filenameLength) << "\n";

    // Tell the server the filename we're about to give it. If the server doesn't supply a filename, this value will be used.
    write(fd_fileout, &filenameLength, sizeof(filenameLength)); // Tells the client how much characters the filename is
    write(fd_fileout, name.data(), filenameLength); // And the actual filename data.
  }
  else if (sAction == "copy")
  {
    if (argc != 5)
    {
      print_usage();
      return -1;
    }

    fd_filein = open(argv[3], O_RDONLY);

    if (fd_filein < 0)
    {
      perror("Unable to open input file");
      return -1;
    }

    fstat(fd_filein, &sb);
    filesize = sb.st_size;
    cout << "filesize: " << filesize << endl;

    fd_fileout = open(argv[4], O_WRONLY | O_CREAT, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);

    if (fd_fileout < 0)
    {
      perror("Unable to open output file");
      return -1;
    }
  }
  else
  {
    print_usage();
    return -1;
  }

  cout << "starting " << sMode << " " << sAction << " of "
       << filesize << " bytes (" << filesizeString(filesize) << ")" << endl;

  if (sMode == "rw")
  {
    transferred = sendfile_rw(fd_fileout, fd_filein, filesize);
  }
  else if (sMode == "sendfile")
  {
    transferred = sendfile_sendfile(fd_fileout, fd_filein, filesize);
  }
  else if (sMode == "splice")
  {
    transferred = sendfile_splice(fd_fileout, fd_filein, filesize);
  }

  cout << " - Done " << filesize << " bytes (" << filesizeString(filesize) << ")" << endl;

  close(fd_fileout);
  close(fd_filein);

  return 0;
}
