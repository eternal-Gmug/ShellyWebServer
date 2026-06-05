#include <sys/epoll.h>
#include <fcntl.h>      // manipulate file descriptors
#include <unistd.h>     // close function in POSIX API
#include <sys/socket.h> // send function for send network message
#include <cstring>      // memset and strerror functions for string operations
#include <cerrno>       // errno for error handling

#include "signal.h"
#include "../include/log.h"

int* SignalHandler::signal_pipefd = nullptr; // Initialize the static member variable for signal pipe file descriptors
int SignalHandler::signal_epoll_fd = -1;     // Initialize the static member variable

void SignalHandler::init(int* pipefd, int epoll_fd) {
    signal_pipefd = pipefd;
    signal_epoll_fd = epoll_fd;
}

int SignalHandler::setNonBlocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);     // get the fd's status flag
    if (old_option < 0) {
        LOG_ERROR("Failed to get file descriptor flags: {}", strerror(errno));
        return -1;
    }
    int new_option = old_option | O_NONBLOCK; // add non-blocking flag
    if (fcntl(fd, F_SETFL, new_option) < 0) {
        LOG_ERROR("Failed to set file descriptor flags: {}", strerror(errno));
        return -1;
    }
    return old_option; // Return the old flags in case the caller wants to restore them later
}

void SignalHandler::addFd(int epollfd, int fd, bool one_shot, int trigger_mode) {
    // register the file to epoll event
    epoll_event event{};
    event.data.fd = fd;
    if (trigger_mode == 1) {
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP; // Edge-triggered mode with read hang-up detection
    }
    else {
        event.events = EPOLLIN | EPOLLRDHUP; // Level-triggered mode with read hang-up detection
    }
    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }
    // add fd to epoll instance
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event) < 0) {
        LOG_ERROR("Failed to add file descriptor {} to epoll: {}", fd, strerror(errno));
        return;
    }
    setNonBlocking(fd); // Set the file descriptor to non-blocking mode after adding it to the epoll instance
}

void SignalHandler::signalHandler(int sig) {
    // save original errno to avoid being modified by the signal handler
    int save_errno = errno;
    int msg = sig;
    // write the signal number to the pipe
    if (signal_pipefd) {
        // send(signal_pipefd[1], reinterpret_cast<char*>(&msg), 1, 0); // Send the signal number through the write end of the pipe
        write(signal_pipefd[1], reinterpret_cast<char*>(&msg), 1); // Write the signal number to the pipe for the main loop to read and handle
    }
    errno = save_errno; // Restore the original errno before returning
}

bool SignalHandler::addSignal(int sig, void (*handler)(int), bool restart) {
    struct sigaction sa {};
    memset(&sa, '\0', sizeof(sa)); // Clear the sigaction structure
    sa.sa_handler = handler; // Set the signal handler function
    if (restart) {
        sa.sa_flags |= SA_RESTART; // Automatically restart interrupted system calls if restart is true
    }
    sigfillset(&sa.sa_mask); // Block all signals during the execution of the signal handler
    if (sigaction(sig, &sa, nullptr) < 0) { // Register the signal handler for the specified signal
        LOG_ERROR("Failed to register signal handler for signal {}: {}", sig, strerror(errno));
        return false;
    }
    return true;
}

void SignalHandler::showError(int connfd, const char* msg) {
    if (!msg) {
        msg = "Unknown error"; // Use a default error message if the provided message is null
    }
    send(connfd, msg, strlen(msg), 0); // Send the error message to the client through the connection file descriptor
    close(connfd); // Close the connection after sending the error message
}