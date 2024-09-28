#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/util.h>
#include <event2/bufferevent.h>
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <string>
#include <memory>
#include <unordered_map>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <atomic>
#include <vector>

using namespace std;

struct st_recvmesg
{
    struct evhttp_request* req;
    string message;

    st_recvmesg(evhttp_request* r, const std::string& msg) : req(r), message(msg) {
        printf("构造了报文。\n");
    }
};

queue<shared_ptr<st_recvmesg>> m_rq;
mutex m_mutex_rq;
condition_variable m_cond_rq;

queue<shared_ptr<st_recvmesg>> m_sq;
mutex m_mutex_sq;
int m_sendpipe[2] = {0};

std::atomic<bool> g_stop{false};
struct evhttp* http_server = nullptr;

void inrq(struct evhttp_request* req, string &message)
{
    shared_ptr<st_recvmesg> ptr = make_shared<st_recvmesg>(req, message);

    lock_guard<mutex> lock(m_mutex_rq);
    m_rq.push(ptr);
    cout<<"inrq数量: "<<m_rq.size()<<endl;
    m_cond_rq.notify_one();
}


void insq(struct evhttp_request* req, string &message)
{
    {
        shared_ptr<st_recvmesg> ptr = make_shared<st_recvmesg>(req, message);
        lock_guard<mutex> lock(m_mutex_sq);
        m_sq.push(ptr);
    }
    write(m_sendpipe[1], (char*)"o", 1);
}

static void http_request_handler(struct evhttp_request* req, void* arg) {
    struct evkeyvalq* headers = evhttp_request_get_input_headers(req);
    cout << "有新连接：";
    std::cout << "Headers: " << headers << std::endl;
    cout << "uri:" << evhttp_request_get_uri(req) << endl;
    string uri = evhttp_request_get_uri(req);
    inrq(req, uri);
}

void work_thread(int id)
{
    while (!g_stop)
    {
        shared_ptr<st_recvmesg> ptr;

        {
            unique_lock<mutex> lock(m_mutex_rq);
            while (m_rq.empty() && !g_stop)
            {
                m_cond_rq.wait(lock);
            }
            if (g_stop) break;
            ptr = m_rq.front(); m_rq.pop();
        }

        printf("工作线程（%d）请求：sock=%d,mesg=%s\n", id, ptr->req, ptr->message.c_str());

        string sendbuf = "ok";
        insq(ptr->req, sendbuf);
        cout<<"insq数量: "<<m_sq.size()<<endl;
       // write(m_sendpipe[1], (char*)"o", 1);
    }
}

void send_http_response() {
    cout << "开始发送函数" << endl;
    shared_ptr<st_recvmesg> ptr;
    {
        unique_lock<mutex> lock(m_mutex_sq);
        if (!m_sq.empty()) {
            ptr = m_sq.front(); m_sq.pop();
        }
    }
    if (ptr) {
        cout << "发送http响应:" << ptr->message << endl;
        
        struct evbuffer *evb = evbuffer_new();
        if (evb == NULL) {
            cerr << "创建evbuffer失败" << endl;
            return;
        }

        evbuffer_add_printf(evb, "%s", ptr->message.c_str());
        
        evhttp_add_header(evhttp_request_get_output_headers(ptr->req),
                          "Content-Type", "text/plain; charset=utf-8");
        
        evhttp_send_reply(ptr->req, HTTP_OK, "OK", evb);

        evbuffer_free(evb);
        // 不要在这里释放 ptr->req，libevent 会处理它
    }
    cout << "发送http响应结束" << endl;
}
void receive_thread(struct event_base* base) {
    evhttp_set_gencb(http_server, http_request_handler, NULL);
    std::cout << "HTTP服务器在8081端口启动。\n";
    event_base_dispatch(base);
}

void send_thread(struct event_base* base) {
    cout<<"启动发送线程"<<endl;
    struct event* pipe_event = event_new(base, m_sendpipe[0], EV_READ | EV_PERSIST,
    [](evutil_socket_t fd, short events, void* arg) {
        cout << "发送线程收到管道事件" << endl;
        
        // 读取管道中的数据（即使我们不使用它，也需要读取以清空管道）
        char buf[1];
        read(fd, buf, sizeof(buf));
        
        send_http_response();
    }, NULL);

    if (!pipe_event || event_add(pipe_event, NULL) < 0) {
        cerr << "无法创建/添加管道事件" << endl;
        return;
    }

    cout << "管道事件已设置" << endl;

    // 在主循环之前
    while (!g_stop) {
        this_thread::sleep_for(chrono::milliseconds(100));
    }

    event_free(pipe_event);

}

int main() {
    struct event_base* base = event_base_new();
    if (!base) {
        std::cerr << "无法初始化libevent！\n";
        return 1;
    }

    http_server = evhttp_new(base);
    if (!http_server) {
        std::cerr << "无法创建新的evhttp。\n";
        event_base_free(base);
        return 1;
    }

    if (evhttp_bind_socket(http_server, "0.0.0.0", 8081) != 0) {
        std::cerr << "无法绑定到8081端口。\n";
        evhttp_free(http_server);
        event_base_free(base);
        return 1;
    }

    if (pipe(m_sendpipe) == -1) {
        std::cerr << "创建管道失败\n";
        return 1;
    }

    std::thread receiver(receive_thread, base);
    
    std::vector<std::thread> workers;
    for (int i = 0; i < 3; ++i) {
        workers.emplace_back(work_thread, i);
    }
    
    std::thread sender(send_thread, base);

    std::cout << "按回车键退出...\n";
    std::cin.get();

    g_stop = true;
    event_base_loopbreak(base);


    receiver.join();
    for (auto& worker : workers) {
        worker.join();
    }
    sender.join();

    close(m_sendpipe[0]);
    close(m_sendpipe[1]);
    evhttp_free(http_server);
    event_base_free(base);

    return 0;
}
