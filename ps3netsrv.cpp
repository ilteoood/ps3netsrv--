/* 
    Based on c implementation of spender
    Rewritten in c++
    Use as you please
*/

#include <array>
#include <future>
#include <algorithm>
#include <cinttypes>
#include <csignal>
#include <fstream>
#include <iostream>
#include <type_traits>
#include <fcntl.h>
#include <sys/stat.h>

#include "utils/fileoperations.h"
#include "utils/socket.h"

#define DEFAULT_PORT 38008
#define LOWEST_PORT 1024

static bool setSignalHandlers();

using namespace utils;

uint64_t htonll(uint64_t val)
{
#if __BYTE_ORDER == __LITTLE_ENDIAN
    return htonl(val >> 32) | ((uint64_t)htonl((uint32_t)val) << 32);
#else
    return val;
#endif
}

uint64_t ntohll(uint64_t val)
{
    return htonll(val);
}

namespace std
{
    template<typename T, typename ...Args>
    std::unique_ptr<T> make_unique(Args&& ...args)
    {
        return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
    }
}

enum class CommandCode
{
    OpenFileForReading      = 0x1224,
    ReadFile                = 0x1225,
    CustomReadFile          = 0x1226,
    ReadShortFile           = 0x1227,
    OpenFileForWriting      = 0x1228,
    WriteToFile             = 0x1229,
    OpenDirectory           = 0x122a,
    ListDirectoryEntryShort = 0x122b,
    DeleteFile              = 0x122c,
    MakeDirectory           = 0x122d,
    RemoveDirectory         = 0x122e,
    ListDirectoryEntryLong  = 0x122f,
    GetFileStats            = 0x1230,
    GetDirectorySize        = 0x1231
};

struct Command
{
    uint16_t code;
    uint16_t size;
    uint32_t count;
    uint64_t offset;
} __attribute__((packed));

struct FileReply
{
    uint64_t size;
    uint64_t mtime;
    uint64_t ctime;
    uint64_t atime;
    uint8_t  isDirectory;
} __attribute__((packed));

struct FileReplyShort
{
    uint64_t size;
    uint16_t nameLength;
    uint8_t  isDirectory;
} __attribute__((packed));

struct FileReplyLong
{
    uint64_t size;
    uint64_t mtime;
    uint64_t ctime;
    uint64_t atime;
    uint16_t nameLength;
    uint8_t  isDirectory;
} __attribute__((packed));

class Ps3Client
{
public:
    Ps3Client(const std::string& rootPath, Socket&& sock)
    : m_rootPath(rootPath)
    , m_Socket(std::move(sock))
    {
        m_Socket.setNoDelayOption();
    }

    std::string getAddress() const
    {
        return m_Socket.getAddress();
    }

    void openFileForReading()
    {
        std::pair<uint64_t, uint64_t> reply {-1, 0};

        try
        {
            if (m_ReadFile.is_open())
            {
                m_ReadFile.close();
            }

            auto path = readFilePath();
            m_ReadFile.open(path);
            if (m_ReadFile.is_open())
            {
                auto info       = fileops::getFileInfo(path);
                reply.first     = htonll(info.sizeInBytes);
                reply.second    = htonll(info.modifyTime);
            }
        }
        catch (std::exception& e)
        {
            std::cerr << e.what() << std::endl;
        }

        m_Socket.write(&reply, sizeof(reply));
    }

    void getFileStats()
    {
        FileReply reply;

        try
        {
            auto info = fileops::getFileInfo(readFilePath());

            reply.size          = htonll(info.sizeInBytes);
            reply.atime         = htonll(info.accessTime);
            reply.ctime         = htonll(info.createTime);
            reply.mtime         = htonll(info.modifyTime);
            reply.isDirectory   = info.type == fileops::FileSystemEntryType::Directory;
        }
        catch (std::exception& e)
        {
            std::cerr << e.what() << std::endl;
            reply.size = -1;
        }
        
        m_Socket.write(&reply, sizeof(reply));
    }

    void readFile()
    {
        throwOnBadReadFile();

        m_ReadFile.seekg(m_Command.offset, std::ios::beg);

        uint32_t bytesToRead = m_Command.count;
        while (bytesToRead > 0)
        {
            auto bufSize = m_BufferSize;
            uint32_t size = std::min(bytesToRead, bufSize);
            m_ReadFile.read(reinterpret_cast<char*>(m_Buffer.data()), size);
            throwOnBadReadFileStatus();
            m_Socket.write(m_Buffer.data(), size);
            bytesToRead -= size;
        }
    }

    void customReadFile()
    {
        std::cout << __FUNCTION__ << std::endl;

        throwOnBadReadFile();
        uint64_t offset = 2352 * m_Command.count;
        uint32_t chunks = *reinterpret_cast<uint32_t*>((&m_Command.offset) + sizeof(uint32_t));

        if ((chunks * m_ChunkSize) > m_BufferSize)
        {
            throw std::logic_error("Too many chunks requested");
        }

        char* pCurrent = reinterpret_cast<char*>(m_Buffer.data());
        for (uint32_t i = 0; i < chunks; ++i)
        {
            m_ReadFile.seekg(offset + 24, std::ios::beg);
            m_ReadFile.read(pCurrent, m_ChunkSize);
            pCurrent += m_ChunkSize;
            offset += 2352;
        }

        m_Socket.write(m_Buffer.data(), chunks * m_ChunkSize);

        std::cout << __FUNCTION__ << " done" << std::endl;
    }

    void readShortFile()
    {
        throwOnBadReadFile();

        if (m_Command.count > m_BufferSize)
        {
            throw std::logic_error("Short file size is larger then buffer size");
        }
        
        m_ReadFile.seekg(m_Command.offset, std::ios::beg);
        m_ReadFile.read(reinterpret_cast<char*>(m_Buffer.data()), m_Command.count);

        writeNumeric(static_cast<uint32_t>(htonl(m_ReadFile.gcount())));
        m_Socket.write(m_Buffer.data(), m_ReadFile.gcount());
    }

    void openFileForWriting()
    {
        if (m_WriteFile.is_open())
        {
            m_WriteFile.close();
        }
        
        m_WriteFile.open(readFilePath(), std::ios::binary);
        m_WriteFile.is_open() ? writeSuccessReply() : writeFailureReply();
    }

    void writeToFile()
    {
        throwOnBadWriteFile();

        if (m_Command.count > m_BufferSize)
        {
            throw std::logic_error("Data to write is larger then buffer size");
        }
        
        m_Socket.read(m_Buffer.data(), m_Command.count);
        auto pos = m_WriteFile.tellp();
        m_WriteFile.write(reinterpret_cast<char*>(m_Buffer.data()), m_Command.count);
        throwOnBadWriteFileStatus();

        writeNumeric(htonl(static_cast<uint32_t>(m_WriteFile.tellp() - pos)));
    }

    void deleteFile()
    {
        filesystemOperation([this] () {
            fileops::deleteFile(readFilePath());
            writeSuccessReply();
        });
    }

    void openDirectory()
    {
        std::cout << __FUNCTION__ << std::endl;
        
        filesystemOperation([this] () {
            m_Directory = std::make_unique<fileops::Directory>(readFilePath());
            m_DirIterator = fileops::FileSystemIterator(*m_Directory);
            writeSuccessReply();
        });
        
        std::cout << __FUNCTION__ << " done" << std::endl;
    }

    void makeDirectory()
    {
        filesystemOperation([this] () {
            fileops::createDirectory(readFilePath());
            writeSuccessReply();    
        });
    }

    void removeDirectory()
    {
        filesystemOperation([this] () {
            fileops::deleteDirectory(readFilePath());
            writeSuccessReply();
        });
    }

    void getDirectorySize()
    {
        filesystemOperation([this] () {
            writeNumeric(htonll(fileops::calculateDirectorySize(readFilePath())));
        });
    }

    void listDirectoryEntryShort()
    {
        std::cout << __FUNCTION__ << std::endl;
        
        if (!m_Directory)
        {
            throw std::logic_error("No directory was opened before listing request");
        }

        try
        {
            FileReplyShort reply;
            memset(&reply, 0, sizeof(reply));

            if (m_DirIterator == end(*m_Directory))
            {
                m_Directory.reset();
                reply.size = htonll(-1LL);
                m_Socket.write(&reply, sizeof(reply));
                return;
            }
            else
            {
                auto name = fileops::getFileName(m_DirIterator->path());
                reply.isDirectory   = (m_DirIterator->type() == fileops::FileSystemEntryType::Directory) ? 1 : 0;
                reply.size          = htonll(reply.isDirectory ? 0 : fileops::getFileSize(m_DirIterator->path()));
                reply.nameLength    = htons(name.size());

                m_Socket.write(&reply, sizeof(FileReplyShort));
                m_Socket.write(&name[0], name.size());
            }
        }
        catch (std::logic_error& e)
        {
            std::cerr << e.what() << std::endl;

            FileReplyShort reply;
            reply.size = htonll(-1LL);
            m_Socket.write(&reply, sizeof(FileReplyShort));
        }

        ++m_DirIterator;
        
        std::cout << __FUNCTION__ << " done" << std::endl;
    }

    void listDirectoryEntryLong()
    {
        if (!m_Directory)
        {
            throw std::logic_error("No directory was opened before listing request");
        }

        try
        {
            FileReplyLong reply;
            memset(&reply, 0, sizeof(reply));

            if (m_DirIterator == end(*m_Directory))
            {
                m_Directory.reset();
                reply.size = htonll(-1LL);
                m_Socket.write(&reply, sizeof(reply));
                return;
            }
            else
            {
                auto info = fileops::getFileInfo(m_DirIterator->path());
                
                auto name = fileops::getFileName(m_DirIterator->path());
                reply.isDirectory   = (m_DirIterator->type() == fileops::FileSystemEntryType::Directory) ? 1 : 0;
                reply.size          = htonll(reply.isDirectory ? 0 : info.sizeInBytes);
                reply.nameLength    = htons(name.size());
                reply.mtime         = htonll(info.modifyTime);
                reply.ctime         = htonll(info.createTime);
                reply.atime         = htonll(info.accessTime);

                m_Socket.write(&reply, sizeof(reply));
                m_Socket.write(&name[0], name.size());
            }
        }
        catch (std::logic_error& e)
        {
            std::cerr << e.what() << std::endl;

            FileReplyLong reply;
            reply.size = htonll(-1LL);
            m_Socket.write(&reply, sizeof(FileReplyLong));
        }

        ++m_DirIterator;
    }

    void run()
    {
        try
        {
            for (;;)
            {
                if (m_Socket.read(&m_Command, sizeof(m_Command)) != sizeof(m_Command))
                    break;
                
                m_Command.code    = ntohs(m_Command.code);
                m_Command.size    = ntohs(m_Command.size);
                m_Command.count   = ntohl(m_Command.count);
                m_Command.offset  = htonll(m_Command.offset);

                switch (static_cast<CommandCode>(m_Command.code))
                {
                case CommandCode::OpenFileForReading:       openFileForReading();                           break;
                case CommandCode::ReadFile:                 readFile();                                     break;
                case CommandCode::CustomReadFile:           customReadFile();                               break;
                case CommandCode::ReadShortFile:            readShortFile();                                break;
                case CommandCode::OpenFileForWriting:       openFileForWriting();                           break;
                case CommandCode::WriteToFile:              writeToFile();                                  break;
                case CommandCode::OpenDirectory:            openDirectory();                                break;
                case CommandCode::ListDirectoryEntryShort:  listDirectoryEntryShort();                      break;
                case CommandCode::DeleteFile:               deleteFile();                                   break;
                case CommandCode::MakeDirectory:            makeDirectory();                                break;
                case CommandCode::RemoveDirectory:          removeDirectory();                              break;
                case CommandCode::ListDirectoryEntryLong:   listDirectoryEntryLong();                       break;
                case CommandCode::GetFileStats:             getFileStats();                                 break;
                case CommandCode::GetDirectorySize:         getDirectorySize();                             break;
                default:
                    std::cerr << "Unknown command: " << std::hex << m_Command.code << std::endl;
                    throw std::logic_error("Unknown command");
                    break;
                }
            }
        }
        catch (std::exception& e)
        {
            m_Socket.close();
            std::cerr << e.what() << std::endl;
        }
    }

private:
    template <typename T>
    void writeNumeric(T value)
    {
        m_Socket.write(&value, sizeof(T));
    }

    std::string readFilePath()
    {
        return fileops::combinePath(m_rootPath, m_Socket.readString(m_Command.size));
    }

    void writeSuccessReply()
    {
        writeNumeric<uint32_t>(0);
    }

    void writeFailureReply()
    {
        writeNumeric<uint32_t>(htonl(-1));
    }
    
    void filesystemOperation(std::function<void()> func)
    {
        try
        {
            func();
        }
        catch (std::exception& e)
        {
            std::cerr << e.what() << std::endl;
            writeFailureReply();
        }
    }

    void throwOnBadReadFile()
    {
        if (!m_ReadFile.is_open())
        {
            throw std::logic_error("Invalid file handle for reading");
        }
    }

    void throwOnBadReadFileStatus()
    {
        if (m_ReadFile.fail())
        {
            throw std::logic_error("File is not ok for reading");
        }
    }

    void throwOnBadWriteFile()
    {
        if (!m_WriteFile.is_open())
        {
            throw std::logic_error("Invalid file handle for writing");
        }
    }

    void throwOnBadWriteFileStatus()
    {
        if (m_WriteFile.fail())
        {
            throw std::logic_error("File is not ok for writing");
        }
    }

    std::string                                 m_rootPath;
    Socket                                      m_Socket;
    Command                                     m_Command;
    
    std::ifstream                               m_ReadFile;
    std::ofstream                               m_WriteFile;
    std::unique_ptr<fileops::Directory>         m_Directory;
    fileops::FileSystemIterator                 m_DirIterator;

    static constexpr uint32_t                   m_BufferSize = 4 * 1024 * 1024;
    static constexpr uint32_t                   m_ChunkSize = 2048;
    std::array<uint8_t, m_BufferSize>           m_Buffer;
};

class Ps3Server
{
public:
    Ps3Server(const std::string& rootPath, uint32_t port)
    : m_rootPath(rootPath)
    {
        m_Socket.setReuseAddressOption();
        m_Socket.startListening(port);
    }

    void run()
    {
        std::cout << "Waiting for client..." << std::endl;
        for (;;)
        {
            try
            {
                m_Clients.push_back(std::make_unique<Ps3Client>(m_rootPath, m_Socket.accept()));
                Ps3Client* client = m_Clients.back().get();
                
                auto task = std::thread([&client] () { client->run(); });
                std::cout << "Connection from " << client->getAddress() << std::endl;
                task.detach();
            }
            catch (std::exception& e)
            {
                std::cerr << "Failed to create client: " << e.what() << std::endl;
            }
        }
    }

private:
    std::string                                 m_rootPath;
    Socket                                      m_Socket;
    std::vector<std::unique_ptr<Ps3Client>>     m_Clients;
};

void usage(const std::string& execName)
{
    std::cout << "Usage: " << execName << " [-d] [-p port] [-w whitelist] rootdirectory" << std::endl
              << "Default port: " << DEFAULT_PORT << std::endl
              << "Whitelist: x.x.x.x, where x is 0-255 or * (e.g 192.168.1.* to allow only connections from 192.168.1.0-192.168.1.255)" << std::endl;
}

int main(int argc, char *argv[])
{
    uint32_t    port{DEFAULT_PORT};
    bool        daemonize{false};

    if (argc < 2)
    {
        usage(argv[0]);
        return -1;
    }
        
    int32_t opt;
    while ((opt = getopt(argc, argv, "p:w:d")) != -1)
    {
        switch (opt)
        {
        case '\x01':
            std::cout << "optarg=" << optarg << std::endl;
            break;
        case 'd':
            daemonize = true;
            break;
        case 'p':
            port = std::stoi(optarg);
            if (port < LOWEST_PORT || port > 65535)
            {
                std::cerr << "Port must be in " << LOWEST_PORT << "-65535 range." << std::endl;
                return -1;
            }
            break;
        default:
            usage(argv[0]);
            return -1;
        }
    }

    if (daemonize)
    {
        pid_t pid{fork()};
        if (pid < 0)
        {
            std::cerr << "Unable to daemonize." << std::endl;
            return -1;
        }

        if (pid > 0)
        {
            return 0;
        }

        umask(0);

        pid_t sid{setsid()};
        if (sid < 0)
        {
            std::cerr << "Failed to get session id." << std::endl;
            return -1;   
        }

        auto fd = open("/dev/null", O_RDWR, 0);
        if (fd != -1)
        {
            dup2(fd, STDIN_FILENO);
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO)
            {
                close(fd);
            }
        }
    }

    setSignalHandlers();
    utils::fileops::changeDirectory(argv[optind]);

    Ps3Server server(argv[optind], port);
    server.run();
}

static void sigterm(int signo)
{
    std::cout << "Terminated" << std::endl;
    exit(1);
}

static bool setSignalHandlers()
{
    struct sigaction sa;

    sa.sa_flags = 0;
    sa.sa_handler = sigterm;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGQUIT);
    sigaddset(&sa.sa_mask, SIGTERM);

    if (sigaction(SIGINT, &sa, nullptr) < 0)
    {
        std::stringstream ss;
        ss << "Can't catch SIGINT: " << strerror(errno) << std::endl;
        throw std::logic_error(ss.str().c_str());
    }

    sa.sa_flags = 0;
    sa.sa_handler = sigterm;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGINT);
    sigaddset(&sa.sa_mask, SIGTERM);

    if (sigaction(SIGQUIT, &sa, nullptr) < 0)
    {
        std::stringstream ss;
        ss << "Can't catch SIGQUIT: " << strerror(errno) << std::endl;
        throw std::logic_error(ss.str().c_str());
    }

    sa.sa_handler = sigterm;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGINT);
    sigaddset(&sa.sa_mask, SIGQUIT);
    sa.sa_flags = 0;

    if (sigaction(SIGTERM, &sa, nullptr) < 0)
    {
        std::stringstream ss;
        ss << "Can't catch SIGTERM: " << strerror(errno) << std::endl;
        throw std::logic_error(ss.str().c_str());
    }

    return true;
}