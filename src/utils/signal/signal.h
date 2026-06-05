#ifndef SIGNAL_H
#define SIGNAL_H

#include <csignal>

/// @brief Signal processing and IO auxiliary tools
/// - signal registration
/// - signal handling
/// - file descriptor management
/// - error handling
class SignalHandler
{
public:
    SignalHandler() = default;
    ~SignalHandler() = default;

    /// @brief initialize the signal handler by setting up signal handlers for SIGINT and SIGTERM, and creating a pipe for signal handling
    void init(int* pipefd, int epoll_fd);

    // set the file descriptor to non-blocking mode
    static int setNonBlocking(int fd);

    // add a file descriptor to the epoll instance with the specified trigger mode and one-shot behavior
    static void addFd(int epollfd, int fd, bool one_shot, int trigger_mode);

    static void signalHandler(int sig); // Static signal handler function to be registered with signal() for handling signals

    static bool addSignal(int sig, void (*handler)(int), bool restart = true); // Static function to register a signal handler for a specific signal, with an option to automatically restart interrupted system calls

    static void showError(int connfd, const char* msg); // Static function to print an error message to standard output, used for error handling in signal processing

    // static member variable to be used for signal handlering
    static int* signal_pipefd; // Pipe file descriptors for signal handling, where signal_pipefd[0] is the read end and signal_pipefd[1] is the write end
    static int signal_epoll_fd; // Epoll file descriptor for monitoring the signal pipe
};

#endif // SIGNAL_H