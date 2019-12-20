#include <QObject>
#include <QSocketNotifier>
#include <QCoreApplication>
#include <QList>

#include <csignal>
#include <cstdint>
#include <cstdlib> // for free
#include <iostream>
#include <sys/types.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/socket.h>
#endif

using namespace std;

#include "compat.h"
#include "mythlogging.h"
#include "exitcodes.h"
#include "signalhandling.h"

int SignalHandler::s_sigFd[2];
volatile bool SignalHandler::s_exit_program = false;
QMutex SignalHandler::s_singletonLock;
SignalHandler *SignalHandler::s_singleton;

// We may need to write out signal info using just the write() function
// so we create an array of C strings + measure their lengths.
#define SIG_STR_COUNT 256
char *sig_str[SIG_STR_COUNT];
uint sig_str_len[SIG_STR_COUNT];

static void sig_str_init(int sig, const char *name)
{
    if (sig < SIG_STR_COUNT)
    {
        char line[128];

        if (sig_str[sig])
            free(sig_str[sig]);
        snprintf(line, 128, "Handling %s\n", name);
        line[127] = '\0';
        sig_str[sig]     = strdup(line);
        sig_str_len[sig] = strlen(line);
    }
}

static void sig_str_init(void)
{
    for (int i = 0; i < SIG_STR_COUNT; i++)
    {
        sig_str[i] = nullptr;
        sig_str_init(i, qPrintable(QString("Signal %1").arg(i)));
    }
}

QList<int> SignalHandler::s_defaultHandlerList;

SignalHandler::SignalHandler(QList<int> &signallist, QObject *parent) :
    QObject(parent)
{
    s_exit_program = false; // set here due to "C++ static initializer madness"
    sig_str_init();

#ifndef _WIN32
    m_sigStack = new char[SIGSTKSZ];
    stack_t stack;
    stack.ss_sp = m_sigStack;
    stack.ss_flags = 0;
    stack.ss_size = SIGSTKSZ;

    // Carry on without the signal stack if it fails
    if (sigaltstack(&stack, nullptr) == -1)
    {
        cerr << "Couldn't create signal stack!" << endl;
        delete [] m_sigStack;
        m_sigStack = nullptr;
    }
#endif

    if (s_defaultHandlerList.isEmpty())
        s_defaultHandlerList << SIGINT << SIGTERM << SIGSEGV << SIGABRT
                             << SIGFPE << SIGILL;
#ifndef _WIN32
    s_defaultHandlerList << SIGBUS;
#if ! CONFIG_DARWIN
    s_defaultHandlerList << SIGRTMIN;
#endif

    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, s_sigFd))
    {
        cerr << "Couldn't create socketpair" << endl;
        return;
    }
    m_notifier = new QSocketNotifier(s_sigFd[1], QSocketNotifier::Read, this);
    connect(m_notifier, SIGNAL(activated(int)), this, SLOT(handleSignal()));

    QList<int>::iterator it = signallist.begin();
    for( ; it != signallist.end(); ++it )
    {
        int signum = *it;
        if (!s_defaultHandlerList.contains(signum))
        {
            cerr << "No default handler for signal " << signum << endl;
            continue;
        }

        SetHandlerPrivate(signum, nullptr);
    }
#endif
}

SignalHandler::~SignalHandler()
{
    s_singleton = nullptr;

#ifndef _WIN32
    if (m_notifier)
    {
        ::close(s_sigFd[0]);
        ::close(s_sigFd[1]);
        delete m_notifier;
    }

    QMutexLocker locker(&m_sigMapLock);
    QMap<int, SigHandlerFunc>::iterator it = m_sigMap.begin();
    for ( ; it != m_sigMap.end(); ++it)
    {
        int signum = it.key();
        signal(signum, SIG_DFL);
    }

    m_sigMap.clear();
#endif
}

void SignalHandler::Init(QList<int> &signallist, QObject *parent)
{
    QMutexLocker locker(&s_singletonLock);
    if (!s_singleton)
        s_singleton = new SignalHandler(signallist, parent);
}

void SignalHandler::Done(void)
{
    QMutexLocker locker(&s_singletonLock);
    delete s_singleton;
}


void SignalHandler::SetHandler(int signum, SigHandlerFunc handler)
{
    QMutexLocker locker(&s_singletonLock);
    if (s_singleton)
        s_singleton->SetHandlerPrivate(signum, handler);
}

void SignalHandler::SetHandlerPrivate(int signum, SigHandlerFunc handler)
{
#ifndef _WIN32
    const char *signame = strsignal(signum);
    QString signal_name = signame ?
        QString(signame) : QString("Unknown(%1)").arg(signum);

    bool sa_handler_already_set = false;
    {
        QMutexLocker locker(&m_sigMapLock);
        sa_handler_already_set = m_sigMap.contains(signum);
        if (m_sigMap.value(signum, nullptr) && (handler != nullptr))
        {
            LOG(VB_GENERAL, LOG_WARNING,
                QString("Warning %1 signal handler overridden")
                .arg(signal_name));
        }
        m_sigMap[signum] = handler;
    }

    if (!sa_handler_already_set)
    {
        struct sigaction sa {};
        sa.sa_sigaction = SignalHandler::signalHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART | SA_SIGINFO;
        if (m_sigStack)
            sa.sa_flags |= SA_ONSTACK;

        sig_str_init(signum, qPrintable(signal_name));

        sigaction(signum, &sa, nullptr);
    }

    LOG(VB_GENERAL, LOG_INFO, QString("Setup %1 handler").arg(signal_name));
#endif
}

struct SignalInfo {
    int      m_signum;
    int      m_code;
    int      m_pid;
    int      m_uid;
    uint64_t m_value;
};

void SignalHandler::signalHandler(int signum, siginfo_t *info, void *context)
{
    SignalInfo signalInfo;

    (void)context;
    signalInfo.m_signum = signum;
#ifdef _WIN32
    (void)info;
    signalInfo.m_code   = 0;
    signalInfo.m_pid    = 0;
    signalInfo.m_uid    = 0;
    signalInfo.m_value  = 0;
#else
    signalInfo.m_code   = (info ? info->si_code : 0);
    signalInfo.m_pid    = (info ? (int)info->si_pid : 0);
    signalInfo.m_uid    = (info ? (int)info->si_uid : 0);
    signalInfo.m_value  = (info ? *(uint64_t *)&info->si_value : 0);
#endif

    // Keep trying if there's no room to write, but stop on error (-1)
    int index = 0;
    int size  = sizeof(SignalInfo);
    char *buffer = (char *)&signalInfo;
    do {
        int written = ::write(s_sigFd[0], &buffer[index], size);
        // If there's an error, the signal will not be seen be the application,
        // but we can't keep trying.
        if (written < 0)
            break;
        index += written;
        size  -= written;
    } while (size > 0);

    // One must not return from SEGV, ILL, BUS or FPE. When these
    // are raised by the program itself they will immediately get
    // re-raised on return.
    //
    // We also handle SIGABRT the same way. While it is safe to
    // return from the signal handler for SIGABRT doing so means
    // SIGABRT will fail when the UI thread is deadlocked; but
    // SIGABRT is the signal one uses to get a core of a
    // deadlocked program.
    switch (signum)
    {
    case SIGSEGV:
    case SIGILL:
#ifndef _WIN32
    case SIGBUS:
#endif
    case SIGFPE:
    case SIGABRT:
        // clear the signal handler so if it reoccurs we get instant death.
        signal(signum, SIG_DFL);

        // Wait for UI event loop to handle this, however we may be
        // blocking it if this signal occured in the UI thread.
        // Note, can not use usleep() as it is not a signal safe function.
        sleep(1);

        if (!s_exit_program)
        {
            // log something to console.. regular logging should be kaput
            // NOTE:  This needs to use write rather than cout or printf as
            //        we need to stick to system calls that are known to be
            //        signal-safe.  write is, the other two aren't.
            int d = 0;
            if (signum < SIG_STR_COUNT)
                d+=::write(STDERR_FILENO, sig_str[signum], sig_str_len[signum]);
            (void) d; // quiet ignoring return value warning.
        }

        // call the default signal handler.  This will kill the application.
        raise(signum);
        break;
    case SIGINT:
    case SIGTERM:
        // clear the signal handler so if it reoccurs we get instant death.
        signal(signum, SIG_DFL);
        break;
    }
}

void SignalHandler::handleSignal(void)
{
#ifndef _WIN32
    m_notifier->setEnabled(false);

    SignalInfo signalInfo;
    int ret = ::read(s_sigFd[1], &signalInfo, sizeof(SignalInfo));
    bool infoComplete = (ret == sizeof(SignalInfo));
    int signum = (infoComplete ? signalInfo.m_signum : 0);

    if (infoComplete)
    {
        char *signame = strsignal(signum);
        signame = strdup(signame ? signame : "Unknown Signal");
        LOG(VB_GENERAL, LOG_CRIT,
            QString("Received %1: Code %2, PID %3, UID %4, Value 0x%5")
            .arg(signame) .arg(signalInfo.m_code) .arg(signalInfo.m_pid)
            .arg(signalInfo.m_uid) .arg(signalInfo.m_value,8,16,QChar('0')));
        free(signame);
    }

    SigHandlerFunc handler = nullptr;
    bool allowNullHandler = false;

#if ! CONFIG_DARWIN
    if (signum == SIGRTMIN)
    {
        // glibc idiots seem to have made SIGRTMIN a macro that expands to a
        // function, so we can't do this in the switch below.
        // This uses the default handler to just get us here and to ignore it.
        allowNullHandler = true;
    }
#endif

    switch (signum)
    {
    case SIGINT:
    case SIGTERM:
        m_sigMapLock.lock();
        handler = m_sigMap.value(signum, nullptr);
        m_sigMapLock.unlock();

        if (handler)
            handler();
        else
            QCoreApplication::exit(0);
        s_exit_program = true;
        break;
    case SIGSEGV:
    case SIGABRT:
    case SIGBUS:
    case SIGFPE:
    case SIGILL:
        usleep(100000);
        s_exit_program = true;
        break;
    default:
        m_sigMapLock.lock();
        handler = m_sigMap.value(signum, nullptr);
        m_sigMapLock.unlock();
        if (handler)
        {
            handler();
        }
        else if (!allowNullHandler)
        {
            LOG(VB_GENERAL, LOG_CRIT, QString("Received unexpected signal %1")
                .arg(signum));
        }
        break;
    }

    m_notifier->setEnabled(true);
#endif
}

/*
 * vim:ts=4:sw=4:ai:et:si:sts=4
 */
