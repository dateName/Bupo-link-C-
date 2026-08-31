// ============================================================================
//  Ds-Link C++ 高性能版（含网卡选择 + 编号显示 + 默认网关优化）
//  编译: g++ -std=c++17 -O2 -march=native main.cpp -o ds_test.exe -pthread -lws2_32 -liphlpapi -lwinmm
//  运行: 以管理员身份运行 .\ds_test.exe
// ============================================================================

#define _WIN32_WINNT 0x0601
#define WINVER 0x0601

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <random>
#include <functional>
#include <map>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <iphlpapi.h>
    #include <icmpapi.h>
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "iphlpapi.lib")
    #pragma comment(lib, "winmm.lib")
#else
    #include <unistd.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <ifaddrs.h>
    #include <net/if.h>
    #include <signal.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <sys/ioctl.h>
    #include <pthread.h>
#endif

// ---------------------------- 全局控制 ----------------------------
std::atomic<bool> stop_flag(false);
std::atomic<bool> test_running(false);
std::vector<double> g_rtt_samples;
size_t g_packets_sent = 0;
size_t g_packets_recv = 0;
std::string g_target_ip;
int g_ping_interval_ms = 300;

// ---------------------------- 跨平台辅助 ----------------------------
#ifdef _WIN32
    void sleep_ms(int ms) { Sleep(ms); }
    void clear_screen() { system("cls"); }
#else
    void sleep_ms(int ms) { usleep(ms * 1000); }
    void clear_screen() { system("clear"); }
#endif

// ---------------------------- 数据结构 ----------------------------
struct AdapterInfo {
    int index;
    std::string name;
    std::string ip;
    std::string gateway;
    std::string mac;
    bool is_default;
};

// ---------------------------- 系统信息获取 (跨平台) ----------------------------
#ifdef _WIN32
std::vector<AdapterInfo> get_adapters() {
    std::vector<AdapterInfo> list;
    ULONG outBufLen = 0;
    GetAdaptersInfo(nullptr, &outBufLen);
    if (outBufLen == 0) return list;
    std::vector<BYTE> buf(outBufLen);
    PIP_ADAPTER_INFO pAdapter = (PIP_ADAPTER_INFO)buf.data();
    if (GetAdaptersInfo(pAdapter, &outBufLen) != NO_ERROR) return list;

    int idx = 0;
    while (pAdapter) {
        AdapterInfo info;
        info.index = idx++;
        info.name = pAdapter->Description;
        info.ip = pAdapter->IpAddressList.IpAddress.String;
        info.gateway = pAdapter->GatewayList.IpAddress.String;
        info.is_default = (strcmp(info.gateway.c_str(), "0.0.0.0") != 0);
        char mac[20];
        snprintf(mac, sizeof(mac), "%02X-%02X-%02X-%02X-%02X-%02X",
                 pAdapter->Address[0], pAdapter->Address[1], pAdapter->Address[2],
                 pAdapter->Address[3], pAdapter->Address[4], pAdapter->Address[5]);
        info.mac = mac;
        list.push_back(info);
        pAdapter = pAdapter->Next;
    }
    return list;
}

std::string get_dns_servers() {
    FIXED_INFO* pInfo = nullptr;
    ULONG len = 0;
    GetNetworkParams(pInfo, &len);
    if (len == 0) return "未检测到";
    std::vector<BYTE> buf(len);
    pInfo = (FIXED_INFO*)buf.data();
    if (GetNetworkParams(pInfo, &len) != NO_ERROR) return "未检测到";
    std::string dns = pInfo->DnsServerList.IpAddress.String;
    IP_ADDR_STRING* next = pInfo->DnsServerList.Next;
    while (next) {
        dns += ", " + std::string(next->IpAddress.String);
        next = next->Next;
    }
    return dns;
}

std::string get_hostname() {
    char name[256];
    DWORD sz = sizeof(name);
    if (GetComputerNameA(name, &sz)) return std::string(name);
    return "未知";
}

std::string get_os_version() {
    OSVERSIONINFOEX osvi;
    ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
    if (!GetVersionEx((LPOSVERSIONINFO)&osvi)) return "Windows";
    std::string ver = "Windows ";
    if (osvi.dwMajorVersion == 10 && osvi.dwMinorVersion == 0) ver += "11";
    else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 3) ver += "8.1";
    else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 2) ver += "8";
    else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 1) ver += "7";
    else ver += std::to_string(osvi.dwMajorVersion) + "." + std::to_string(osvi.dwMinorVersion);
    return ver;
}
#else
std::vector<AdapterInfo> get_adapters() {
    std::vector<AdapterInfo> list;
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) return list;
    int idx = 0;
    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        AdapterInfo info;
        info.index = idx++;
        info.name = ifa->ifa_name;
        info.ip = inet_ntoa(((struct sockaddr_in*)ifa->ifa_addr)->sin_addr);
        info.gateway = "未获取";
        info.is_default = false;
        info.mac = "XX-XX-XX-XX-XX-XX";
        list.push_back(info);
    }
    freeifaddrs(ifaddr);
    return list;
}

std::string get_dns_servers() {
    FILE* fp = fopen("/etc/resolv.conf", "r");
    if (!fp) return "未检测到";
    char line[256];
    std::string dns;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "nameserver", 10) == 0) {
            char ip[32];
            sscanf(line, "nameserver %s", ip);
            if (!dns.empty()) dns += ", ";
            dns += ip;
        }
    }
    fclose(fp);
    return dns.empty() ? "未检测到" : dns;
}

std::string get_hostname() {
    char name[256];
    if (gethostname(name, sizeof(name)) == 0) return std::string(name);
    return "未知";
}

std::string get_os_version() {
    #ifdef __ANDROID__
        return "Android (Termux)";
    #else
        return "Linux";
    #endif
}
#endif

// ---------------------------- 网络测试引擎 ----------------------------
#ifdef _WIN32
double icmp_ping_win(const std::string& ip, int timeout_ms = 2000) {
    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) return -1.0;
    DWORD ip_addr = inet_addr(ip.c_str());
    if (ip_addr == INADDR_NONE) { IcmpCloseHandle(hIcmp); return -1.0; }
    char data[] = "Ping";
    DWORD reply_size = sizeof(ICMP_ECHO_REPLY) + sizeof(data) + 8;
    std::vector<BYTE> buf(reply_size);
    DWORD result = IcmpSendEcho(hIcmp, ip_addr, data, (DWORD)strlen(data),
                                NULL, buf.data(), reply_size, timeout_ms);
    IcmpCloseHandle(hIcmp);
    if (result == 0) return -1.0;
    PICMP_ECHO_REPLY pReply = (PICMP_ECHO_REPLY)buf.data();
    return (double)pReply->RoundTripTime / 1000.0;
}
#else
double tcp_connect_linux(const std::string& ip, int port = 80, int timeout_ms = 2000) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1.0;
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    auto start = std::chrono::steady_clock::now();
    int ret = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    auto end = std::chrono::steady_clock::now();
    close(sock);
    if (ret != 0) return -1.0;
    return std::chrono::duration<double, std::milli>(end - start).count();
}
#endif

// ---------------------------- 数据包构造 ----------------------------
std::vector<char> build_packet(int type, int size) {
    std::vector<char> buf(size);
    switch(type) {
        case 1:
            for (int i = 0; i < size; ++i) buf[i] = (char)(rand() % 256);
            break;
        case 2:
            std::fill(buf.begin(), buf.end(), 0);
            break;
        case 3:
            if (size < 64) size = 64;
            std::fill(buf.begin(), buf.end(), 0);
            buf[0] = 0x12; buf[1] = 0x34;
            buf[2] = 0x01; buf[3] = 0x00;
            buf[4] = 0x00; buf[5] = 0x01;
            break;
        case 4:
            if (size < 48) size = 48;
            std::fill(buf.begin(), buf.end(), 0);
            buf[0] = 0x1B;
            break;
        case 5: {
            if (size < 128) size = 128;
            std::string ssdp = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: ssdp:all\r\n\r\n";
            int len = std::min(size, (int)ssdp.length());
            std::copy(ssdp.begin(), ssdp.begin() + len, buf.begin());
            break;
        }
        default:
            std::fill(buf.begin(), buf.end(), 0xAA);
    }
    return buf;
}

// ---------------------------- 全局原子统计 ----------------------------
std::atomic<size_t> g_total_bytes(0);

// ---------------------------- UDP 泛洪线程（终极优化） ----------------------------
void udp_flood_worker(const std::string& ip, int port, int packet_type, int packet_size, int sndbuf_kb, int thread_id) {
#ifdef _WIN32
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return;

    int timeout_ms = 10;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout_ms, sizeof(timeout_ms));

    int sndbuf = sndbuf_kb * 1024;
    if (sndbuf < 1024 * 1024) sndbuf = 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&sndbuf, sizeof(sndbuf));

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    DWORD_PTR mask = 1ull << (thread_id % 64);
    SetThreadAffinityMask(GetCurrentThread(), mask);
#else
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int sndbuf = sndbuf_kb * 1024;
    if (sndbuf < 1024 * 1024) sndbuf = 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(thread_id % std::thread::hardware_concurrency(), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#endif

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    addr.sin_port = htons(port);

    std::vector<char> packet = build_packet(packet_type, packet_size);

    size_t local_bytes = 0;
    int send_count = 0;
    const int BATCH = 64;

    while (!stop_flag.load()) {
        for (int i = 0; i < BATCH; ++i) {
            if (stop_flag.load()) break;
            int ret = sendto(sock, packet.data(), packet.size(), 0,
                             (struct sockaddr*)&addr, sizeof(addr));
            if (ret > 0) {
                local_bytes += packet.size();
                ++send_count;
            } else {
#ifdef _WIN32
                if (WSAGetLastError() == WSAEWOULDBLOCK) Sleep(1);
#else
                if (errno == EAGAIN || errno == EWOULDBLOCK) usleep(1000);
#endif
            }
        }
        if (send_count > 0) {
            g_total_bytes += local_bytes;
            local_bytes = 0;
            send_count = 0;
        }
    }
    if (send_count > 0) {
        g_total_bytes += local_bytes;
    }

#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

// ---------------------------- 测试主控 ----------------------------
struct TestConfig {
    std::string target_ip;
    int port_mode;
    std::vector<int> ports;
    int threads;
    int test_duration;
    int packet_type;
    int packet_size;
    int sndbuf_kb;
    int ping_interval_ms;
};

void start_test(const TestConfig& cfg) {
    stop_flag.store(false);
    g_rtt_samples.clear();
    g_packets_sent = 0;
    g_packets_recv = 0;
    g_total_bytes = 0;
    test_running.store(true);

    std::cout << "\n\033[1;32m[测试进行中] 按 Ctrl+C 停止...\033[0m\n";

    std::vector<std::thread> flooders;
    int num_threads = cfg.threads;
    for (int i = 0; i < num_threads; ++i) {
        int port = cfg.ports[ i % cfg.ports.size() ];
        flooders.emplace_back(udp_flood_worker, cfg.target_ip, port,
                              cfg.packet_type, cfg.packet_size, cfg.sndbuf_kb, i);
    }

    auto start_time = std::chrono::steady_clock::now();
    auto last_print = std::chrono::steady_clock::now();

    while (!stop_flag.load()) {
        auto now = std::chrono::steady_clock::now();
        if (cfg.test_duration > 0) {
            if (std::chrono::duration<double>(now - start_time).count() > cfg.test_duration) {
                stop_flag.store(true);
                break;
            }
        }

        double rtt = -1.0;
#ifdef _WIN32
        rtt = icmp_ping_win(cfg.target_ip);
#else
        rtt = tcp_connect_linux(cfg.target_ip, 80);
#endif
        g_packets_sent++;
        if (rtt > 0) {
            g_rtt_samples.push_back(rtt);
            g_packets_recv++;
        }

        if (std::chrono::duration<double>(now - last_print).count() >= 0.5) {
            double elapsed = std::chrono::duration<double>(now - start_time).count();
            double mbps = (g_total_bytes * 8.0) / (elapsed * 1000000.0);
            std::cout << "\r[运行 " << std::fixed << std::setprecision(1) << elapsed
                      << "s]  发送: " << g_packets_sent
                      << "  收: " << g_packets_recv
                      << "  丢包率: " << std::setprecision(1)
                      << (g_packets_sent>0 ? 100.0*(g_packets_sent-g_packets_recv)/g_packets_sent : 0) << "%"
                      << "  当前RTT: " << (rtt>0 ? std::to_string(rtt) : "超时")
                      << " ms  速率: " << std::setprecision(2) << mbps << " Mbps"
                      << std::string(20, ' ') << std::flush;
            last_print = now;
        }

        sleep_ms(cfg.ping_interval_ms);
    }

    stop_flag.store(true);
    for (auto& th : flooders) {
        if (th.joinable()) th.join();
    }

    test_running.store(false);
    std::cout << "\n\n\033[1;33m========== 统计报告 ==========\033[0m\n";
    std::cout << "总发送包数: " << g_packets_sent << "\n";
    std::cout << "接收包数:   " << g_packets_recv << "\n";
    double loss = g_packets_sent>0 ? 100.0*(g_packets_sent-g_packets_recv)/g_packets_sent : 0;
    std::cout << "丢包率:     " << std::fixed << std::setprecision(2) << loss << "%\n";
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
    double mbps = (g_total_bytes * 8.0) / (elapsed * 1000000.0);
    std::cout << "平均速率:   " << std::setprecision(2) << mbps << " Mbps\n";

    if (g_rtt_samples.empty()) {
        std::cout << "无有效 RTT 数据。\n";
        return;
    }
    double sum=0, minv=1e9, maxv=-1e9;
    for (double v : g_rtt_samples) { sum+=v; minv=std::min(minv,v); maxv=std::max(maxv,v); }
    double avg = sum / g_rtt_samples.size();
    double sq=0;
    for (double v : g_rtt_samples) sq += (v-avg)*(v-avg);
    double jitter = std::sqrt(sq / g_rtt_samples.size());
    std::cout << "最小延迟:   " << minv << " ms\n";
    std::cout << "最大延迟:   " << maxv << " ms\n";
    std::cout << "平均延迟:   " << avg << " ms\n";
    std::cout << "抖动 (std): " << jitter << " ms\n";
    std::cout << "================================\n";
}

// ---------------------------- UI 交互 (含网卡选择) ----------------------------
class WizardUI {
private:
    TestConfig cfg;
    std::vector<AdapterInfo> adapters;
    int current_step;   // 0=选择网卡, 1=基本配置, 2=包类型, 3=高级, 4=确认
    int selected_adapter_index;  // 临时存储

    void print_header() {
        std::cout << "\n\033[1;36m┌─ 当前网络环境（只读）──────────────────────\033[0m\n";
        std::cout << "     系统:       " << get_os_version() << "\n";
        std::cout << "     主机名:     " << get_hostname() << "\n";
        std::string default_ip = "未检测到";
        for (auto& a : adapters) {
            if (a.is_default && a.ip != "0.0.0.0") {
                default_ip = a.ip;
                break;
            }
        }
        std::cout << "     IPv4 地址:  " << default_ip << "\n";
        std::string gateway = "未检测到";
        for (auto& a : adapters) {
            if (a.is_default && a.gateway != "0.0.0.0") {
                gateway = a.gateway;
                break;
            }
        }
        std::cout << "     默认网关:   " << gateway << "\n";
        std::cout << "     DNS:        " << get_dns_servers() << "\n";
        std::cout << "     可用网卡:\n";
        for (size_t i = 0; i < adapters.size(); ++i) {
            std::cout << "       [" << i << "] " << adapters[i].name << ": " << adapters[i].ip;
            if (adapters[i].gateway != "0.0.0.0" && adapters[i].gateway != "未获取")
                std::cout << " (网关: " << adapters[i].gateway << ")";
            std::cout << "\n";
        }
        std::cout << "     说明:       本程序不会修改上述网络配置。\n";
        std::cout << "\033[1;36m  └──────────────────────────────────────────\033[0m\n";
    }

    void print_adapter_selection() {
        std::cout << "\n\033[1;33m  ┌─ 选择网卡 ──────────────────────────────────\033[0m\n";
        std::cout << "  （输入 b 返回上一步）\n";
        std::cout << "  请输入要测试的网卡编号（0~" << (adapters.size()-1) << "）: ";
        std::string input;
        std::getline(std::cin, input);
        if (input == "b" || input == "B") {
            current_step--;  // 如果当前是第一步，则退出（目前是第一步，无法再退）
            return;
        }
        try {
            int idx = std::stoi(input);
            if (idx >= 0 && idx < (int)adapters.size()) {
                selected_adapter_index = idx;
                // 设置目标 IP：优先使用网关，如果网关无效则用 IP
                std::string gw = adapters[idx].gateway;
                if (gw != "0.0.0.0" && gw != "未获取" && !gw.empty()) {
                    cfg.target_ip = gw;
                } else {
                    cfg.target_ip = adapters[idx].ip;
                    if (cfg.target_ip == "0.0.0.0") {
                        std::cout << "  [警告] 该网卡 IP 为 0.0.0.0，可能无法通信。\n";
                    }
                }
                std::cout << "  已选择网卡: " << adapters[idx].name << "，目标 IP = " << cfg.target_ip << "\n";
                current_step = 1;  // 进入下一步
            } else {
                std::cout << "  编号超出范围，请重新输入。\n";
            }
        } catch(...) {
            std::cout << "  输入无效，请输入数字或 b。\n";
        }
    }

    void print_basic_config() {
        std::cout << "\n\033[1;33m  ┌─ 基本配置 ────────────────────────────────\033[0m\n";
        std::cout << "  （输入 b 返回上一步）\n";
        std::cout << "  目标 IP 地址【" << cfg.target_ip << "】: ";
        std::string input;
        std::getline(std::cin, input);
        if (input == "b" || input == "B") { current_step--; return; }
        if (!input.empty()) cfg.target_ip = input;

        std::cout << "  端口（单端口如 53，或范围如 1-65535）【"
                  << (cfg.ports.size()==1 ? std::to_string(cfg.ports[0]) : std::to_string(cfg.ports.front())+"-"+std::to_string(cfg.ports.back()))
                  << "】: ";
        std::getline(std::cin, input);
        if (input == "b" || input == "B") { current_step--; return; }
        if (!input.empty()) {
            if (input.find('-') != std::string::npos) {
                int s,e;
                if (sscanf(input.c_str(), "%d-%d", &s, &e) == 2) {
                    cfg.ports.clear();
                    for (int p=s; p<=e && p<=65535; ++p) cfg.ports.push_back(p);
                    cfg.port_mode = 1;
                }
            } else {
                int p = std::stoi(input);
                if (p>=1 && p<=65535) { cfg.ports = {p}; cfg.port_mode = 0; }
            }
        }

        std::cout << "  线程数（建议 8~64）【" << cfg.threads << "】: ";
        std::getline(std::cin, input);
        if (input == "b" || input == "B") { current_step--; return; }
        if (!input.empty()) cfg.threads = std::stoi(input);

        std::cout << "  测试时长/秒（0=无限，Ctrl+C 停止）【" << cfg.test_duration << "】: ";
        std::getline(std::cin, input);
        if (input == "b" || input == "B") { current_step--; return; }
        if (!input.empty()) cfg.test_duration = std::stoi(input);
    }

    void print_packet_type() {
        std::cout << "\n\033[1;33m  ┌─ 数据包类型 ──────────────────────────────\033[0m\n";
        std::cout << "    1. 随机数据（不可压缩，效果最好）\n";
        std::cout << "    2. 全零数据（可被压缩优化，效果差）\n";
        std::cout << "    3. DNS 查询（短包，压路由器 CPU/会话表）\n";
        std::cout << "    4. NTP 请求（短包）\n";
        std::cout << "    5. SSDP 发现（短包，多播）\n";
        std::cout << "    6. 混合模式（随机大包 + DNS + NTP）\n";
        std::cout << "  （输入 b 返回上一步）\n";
        std::cout << "  请选择【" << cfg.packet_type << "】: ";
        std::string input;
        std::getline(std::cin, input);
        if (input == "b" || input == "B") { current_step--; return; }
        if (!input.empty()) {
            int val = std::stoi(input);
            if (val>=1 && val<=6) cfg.packet_type = val;
        }
        if (cfg.packet_type == 6) cfg.packet_size = 1024;
    }

    void print_advanced() {
        std::cout << "\n\033[1;33m  ┌─ 高级配置 ────────────────────────────────\033[0m\n";
        std::cout << "  （输入 b 返回上一步）\n";
        std::cout << "  Socket 发送缓冲区 KB（64=暴露 Bufferbloat, 1024=隐藏延迟）【" << cfg.sndbuf_kb << "】: ";
        std::string input;
        std::getline(std::cin, input);
        if (input == "b" || input == "B") { current_step--; return; }
        if (!input.empty()) cfg.sndbuf_kb = std::stoi(input);

        std::cout << "  Ping 间隔秒（默认 0.3）【" << (cfg.ping_interval_ms/1000.0) << "】: ";
        std::getline(std::cin, input);
        if (input == "b" || input == "B") { current_step--; return; }
        if (!input.empty()) {
            double sec = std::stod(input);
            cfg.ping_interval_ms = (int)(sec * 1000);
        }
    }

    void print_confirm() {
        std::cout << "\n\033[1;33m  ┌─ 配置确认 ────────────────────────────────\033[0m\n";
        std::cout << "     目标:     " << cfg.target_ip << "\n";
        std::cout << "     端口:     ";
        if (cfg.ports.size() == 1) std::cout << "固定端口(" << cfg.ports[0] << ")\n";
        else std::cout << "范围(" << cfg.ports.front() << "-" << cfg.ports.back() << ")\n";
        std::cout << "     线程:     " << cfg.threads << "\n";
        std::cout << "     数据:     ";
        switch(cfg.packet_type) {
            case 1: std::cout << "随机数据(大包)"; break;
            case 2: std::cout << "全零数据"; break;
            case 3: std::cout << "DNS查询"; break;
            case 4: std::cout << "NTP请求"; break;
            case 5: std::cout << "SSDP发现"; break;
            case 6: std::cout << "混合模式"; break;
        }
        std::cout << "\n     SO_SNDBUF:" << cfg.sndbuf_kb << "KB\n";
        std::cout << "     Ping间隔: " << cfg.ping_interval_ms/1000.0 << "s\n";
        std::cout << "     时长:     " << (cfg.test_duration==0 ? "无限 (Ctrl+C 停止)" : std::to_string(cfg.test_duration)+"s") << "\n";
        std::cout << "\n  按 Enter 开始测试，输入 b 重新配置...";
        std::string input;
        std::getline(std::cin, input);
        if (input == "b" || input == "B") { current_step = 0; return; }

        start_test(cfg);
        std::cout << "\n按任意键返回主菜单...";
        std::cin.get();
        current_step = 0;
    }

public:
    WizardUI() {
        cfg.threads = 16;
        cfg.test_duration = 0;
        cfg.packet_type = 1;
        cfg.packet_size = 1400;
        cfg.sndbuf_kb = 1024;
        cfg.ping_interval_ms = 300;
        cfg.port_mode = 0;
        cfg.ports = {53};
        current_step = 0;
        selected_adapter_index = -1;
    }

    void run() {
        adapters = get_adapters();
        if (adapters.empty()) {
            std::cout << "未检测到任何网卡，退出。\n";
            return;
        }
        // 默认选中第一个有有效网关（非0.0.0.0）的网卡
        selected_adapter_index = -1;
        for (size_t i = 0; i < adapters.size(); ++i) {
            if (adapters[i].gateway != "0.0.0.0" && adapters[i].gateway != "未获取" && !adapters[i].gateway.empty()) {
                selected_adapter_index = i;
                cfg.target_ip = adapters[i].gateway;
                break;
            }
        }
        if (selected_adapter_index == -1) {
            // 若没有有效网关，则选第一个有IP的网卡
            for (size_t i = 0; i < adapters.size(); ++i) {
                if (adapters[i].ip != "0.0.0.0" && !adapters[i].ip.empty()) {
                    selected_adapter_index = i;
                    cfg.target_ip = adapters[i].ip;
                    break;
                }
            }
            if (selected_adapter_index == -1) {
                selected_adapter_index = 0;
                cfg.target_ip = adapters[0].ip;
            }
        }

        while (true) {
            clear_screen();
            print_header();

            if (current_step == 0) {
                print_adapter_selection();
                if (current_step < 0) { current_step = 0; continue; }
                if (current_step == 0) continue; // 输入无效则继续
                // 否则 step 变为 1
            }
            if (current_step == 1) {
                print_basic_config();
                if (current_step < 1) { current_step = 0; continue; }
                if (current_step == 1) current_step = 2;
            }
            if (current_step == 2) {
                print_packet_type();
                if (current_step < 2) { current_step = 1; continue; }
                if (current_step == 2) current_step = 3;
            }
            if (current_step == 3) {
                print_advanced();
                if (current_step < 3) { current_step = 2; continue; }
                if (current_step == 3) current_step = 4;
            }
            if (current_step == 4) {
                print_confirm();
                if (current_step < 4) { current_step = 3; continue; }
                // 测试完成后会重置 current_step = 0，循环继续
            }
        }
    }
};

// ---------------------------- 信号处理 ----------------------------
#ifdef _WIN32
BOOL WINAPI ConsoleHandler(DWORD dwCtrlType) {
    if (dwCtrlType == CTRL_C_EVENT && test_running.load()) {
        std::cout << "\n\033[1;31m[!] 收到 Ctrl+C，正在停止测试...\033[0m\n";
        stop_flag.store(true);
        return TRUE;
    }
    return FALSE;
}
#else
void signal_handler(int sig) {
    if (sig == SIGINT && test_running.load()) {
        std::cout << "\n\033[1;31m[!] 收到 Ctrl+C，正在停止测试...\033[0m\n";
        stop_flag.store(true);
    }
}
#endif

// ---------------------------- 主函数 ----------------------------
int main() {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#else
    signal(SIGINT, signal_handler);
#endif

    std::srand((unsigned)std::time(nullptr));

    WizardUI wizard;
    wizard.run();

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}