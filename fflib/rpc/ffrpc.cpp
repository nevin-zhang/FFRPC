#include "rpc/ffrpc.h"
#include "rpc/ffrpc_ops.h"
#include "base/log.h"
#include "net/net_factory.h"

using namespace ff;

#define FFRPC                   "FFRPC"

ffrpc_t::ffrpc_t(string service_name_):
    m_service_name(service_name_),
    m_node_id(0),
    m_callback_id(0),
    m_master_broker_sock(NULL),
    m_bind_broker_id(0)
{
    if (m_service_name.empty())
    {
        char tmp[512];
        sprintf(tmp, "FFRPCClient-%ld-%p", ::time(NULL), this);
        m_service_name = tmp;
    }
}

ffrpc_t::~ffrpc_t()
{
    
}

int ffrpc_t::open(const string& opt_)
{
    net_factory_t::start(1);
    m_host = opt_;

    m_thread.create_thread(task_binder_t::gen(&task_queue_t::run, &m_tq), 1);
            
    //!新版本
    m_ffslot.bind(REGISTER_TO_BROKER_RET, ffrpc_ops_t::gen_callback(&ffrpc_t::handle_broker_reg_response, this))
            .bind(BROKER_TO_CLIENT_MSG, ffrpc_ops_t::gen_callback(&ffrpc_t::handle_call_service_msg, this));

    m_master_broker_sock = connect_to_broker(m_host, BROKER_MASTER_NODE_ID);

    if (NULL == m_master_broker_sock)
    {
        LOGERROR((FFRPC, "ffrpc_t::open failed, can't connect to remote broker<%s>", m_host.c_str()));
        return -1;
    }

    while(m_node_id == 0)
    {
        usleep(1);
        if (m_master_broker_sock == NULL)
        {
            LOGERROR((FFRPC, "ffrpc_t::open failed"));
            return -1;
        }
    }
    singleton_t<ffrpc_memory_route_t>::instance().add_node(m_node_id, this);
    LOGTRACE((FFRPC, "ffrpc_t::open end ok m_node_id[%u]", m_node_id));
    return 0;
}

//! 连接到broker master
socket_ptr_t ffrpc_t::connect_to_broker(const string& host_, uint32_t node_id_)
{
    LOGINFO((FFRPC, "ffrpc_t::connect_to_broker begin...host_<%s>,node_id_[%u]", host_.c_str(), node_id_));
    socket_ptr_t sock = net_factory_t::connect(host_, this);
    if (NULL == sock)
    {
        LOGERROR((FFRPC, "ffrpc_t::register_to_broker_master failed, can't connect to remote broker<%s>", host_.c_str()));
        return sock;
    }
    session_data_t* psession = new session_data_t(node_id_);
    sock->set_data(psession);

    //!新版发送注册消息
    register_to_broker_t::in_t reg_msg;
    reg_msg.service_name = m_service_name;
    reg_msg.node_id = m_node_id;
    msg_sender_t::send(sock, REGISTER_TO_BROKER_REQ, reg_msg);
    return sock;

    //! 发送注册消息给master broker
    if (node_id_ == BROKER_MASTER_NODE_ID)
    {
        register_all_interface(sock);
    }
    //! 发送注册消息给master slave broker
    else
    {
        register_client_to_slave_broker_t::in_t msg;
        msg.node_id = m_node_id;
        msg_sender_t::send(sock, CLIENT_REGISTER_TO_SLAVE_BROKER, msg);
    }
    return sock;
}
//! 投递到ffrpc 特定的线程
static void route_call_reconnect(ffrpc_t* ffrpc_)
{
    ffrpc_->get_tq().produce(task_binder_t::gen(&ffrpc_t::timer_reconnect_broker, ffrpc_));
}
//! 定时重连 broker master
void ffrpc_t::timer_reconnect_broker()
{
    LOGINFO((FFRPC, "ffrpc_t::timer_reconnect_broker begin..."));
    if (NULL == m_master_broker_sock)
    {
        m_master_broker_sock = connect_to_broker(m_host, BROKER_MASTER_NODE_ID);
        if (NULL == m_master_broker_sock)
        {
            LOGERROR((FFRPC, "ffrpc_t::timer_reconnect_broker failed, can't connect to remote broker<%s>", m_host.c_str()));
            //! 设置定时器重连
            m_timer.once_timer(RECONNECT_TO_BROKER_TIMEOUT, task_binder_t::gen(&route_call_reconnect, this));
        }
        else
        {
            LOGWARN((FFRPC, "ffrpc_t::timer_reconnect_broker failed, connect to master remote broker<%s> ok", m_host.c_str()));
        }
    }
    //!检测是否需要连接slave broker
    map<string/*host*/, uint64_t>::iterator it = m_broker_data.slave_broker_data.begin();
    for (; it != m_broker_data.slave_broker_data.end(); ++it)
    {
        uint64_t node_id = it->second;
        string host = it->first;
        if (m_broker_sockets.find(node_id) == m_broker_sockets.end())//!重连
        {
            socket_ptr_t sock = connect_to_broker(host, node_id);
            if (sock == NULL)
            {
                LOGERROR((FFRPC, "ffrpc_t::timer_reconnect_broker failed, can't connect to remote broker<%s>", host.c_str()));
            }
            else
            {
                m_broker_sockets[node_id] = sock;
                LOGWARN((FFRPC, "ffrpc_t::timer_reconnect_broker failed, connect to slave remote broker<%s> ok", host.c_str()));
            }
        }
    }
    LOGINFO((FFRPC, "ffrpc_t::timer_reconnect_broker  end ok"));
}

//!  register all interface
int ffrpc_t::register_all_interface(socket_ptr_t sock)
{
    register_broker_client_t::in_t msg;
    msg.binder_broker_node_id = singleton_t<ffrpc_memory_route_t>::instance().get_broker_node_same_process();
    msg.service_name          = m_service_name;
    
    for (map<string, ffslot_t::callback_t*>::iterator it = m_reg_iterface.begin(); it != m_reg_iterface.end(); ++it)
    {
        msg.msg_names.insert(it->first);
    }
    msg_sender_t::send(sock, BROKER_CLIENT_REGISTER, msg);
    return 0;
}
//! 获取任务队列对象
task_queue_t& ffrpc_t::get_tq()
{
    return m_tq;
}
int ffrpc_t::handle_broken(socket_ptr_t sock_)
{
    m_tq.produce(task_binder_t::gen(&ffrpc_t::handle_broken_impl, this, sock_));
    return 0;
}
int ffrpc_t::handle_msg(const message_t& msg_, socket_ptr_t sock_)
{
    m_tq.produce(task_binder_t::gen(&ffrpc_t::handle_msg_impl, this, msg_, sock_));
    return 0;
}

int ffrpc_t::handle_broken_impl(socket_ptr_t sock_)
{
    //! 设置定时器重练
    if (m_master_broker_sock == sock_)
    {
        m_master_broker_sock = NULL;
    }
    else
    {
        map<uint64_t, socket_ptr_t>::iterator it = m_broker_sockets.begin();
        for (; it != m_broker_sockets.end(); ++it)
        {
            m_broker_sockets.erase(it);
            break;
        }
    }
    sock_->safe_delete();
    m_timer.once_timer(RECONNECT_TO_BROKER_TIMEOUT, task_binder_t::gen(&route_call_reconnect, this));
    return 0;
}

int ffrpc_t::handle_msg_impl(const message_t& msg_, socket_ptr_t sock_)
{
    uint16_t cmd = msg_.get_cmd();
    LOGDEBUG((FFRPC, "ffrpc_t::handle_msg_impl cmd[%u] begin", cmd));

    ffslot_t::callback_t* cb = m_ffslot.get_callback(cmd);
    if (cb)
    {
        try
        {
            ffslot_msg_arg arg(msg_.get_body(), sock_);
            cb->exe(&arg);
            LOGDEBUG((FFRPC, "ffrpc_t::handle_msg_impl cmd[%u] call end ok", cmd));
            return 0;
        }
        catch(exception& e_)
        {
            LOGDEBUG((BROKER, "ffrpc_t::handle_msg_impl exception<%s> body_size=%u", e_.what(), msg_.get_body().size()));
            return -1;
        }
    }
    LOGWARN((FFRPC, "ffrpc_t::handle_msg_impl cmd[%u] end ok", cmd));
    return -1;
}

int ffrpc_t::handle_broker_route_msg(broker_route_t::in_t& msg_, socket_ptr_t sock_)
{
    return trigger_callback(msg_);
}

//! 新版 调用消息对应的回调函数
int ffrpc_t::handle_call_service_msg(broker_route_msg_t::in_t& msg_, socket_ptr_t sock_)
{
    LOGTRACE((FFRPC, "ffrpc_t::handle_call_service_msg msg begin dest_msg_name=%s callback_id=%u", msg_.dest_msg_name, msg_.callback_id));
    
    if (msg_.dest_service_name.empty() == false)
    {
        ffslot_t::callback_t* cb = m_ffslot_interface.get_callback(msg_.dest_msg_name);
        if (cb)
        {
            //ffslot_req_arg arg(msg_.body, msg_.from_node_id, msg_.callback_id, msg_.bridge_route_id, this);
            ffslot_req_arg arg(msg_.body, msg_.from_node_id, msg_.callback_id, 0, this);
            cb->exe(&arg);
            return 0;
        }
        else
        {
            LOGERROR((FFRPC, "ffrpc_t::handle_call_service_msg service=%s and msg_name=%s not found", msg_.dest_service_name, msg_.dest_msg_name));
        }
    }
    else
    {
        ffslot_t::callback_t* cb = m_ffslot_callback.get_callback(msg_.callback_id);
        if (cb)
        {
            //ffslot_req_arg arg(msg_.body, msg_.from_node_id, msg_.callback_id, msg_.bridge_route_id, this);
            ffslot_req_arg arg(msg_.body, 0, 0, 0, this);
            cb->exe(&arg);
            m_ffslot_callback.del(msg_.callback_id);
            return 0;
        }
        else
        {
            LOGERROR((FFRPC, "ffrpc_t::handle_call_service_msg callback_id[%u] or dest_msg=%s not found", msg_.callback_id, msg_.dest_msg_name));
        }
    }
    LOGTRACE((FFRPC, "ffrpc_t::handle_call_service_msg msg end ok"));
    return 0;
}

int ffrpc_t::trigger_callback(broker_route_t::in_t& msg_)
{
    LOGTRACE((FFRPC, "ffrpc_t::handle_broker_route_msg msg_id[%u],callback_id[%u] begin", msg_.msg_id, msg_.callback_id));
    try
    {
        if (msg_.msg_id == 0)//! msg_id 为0表示这是一个回调的消息，callback_id已经有值
        {
            ffslot_t::callback_t* cb = m_ffslot_callback.get_callback(msg_.callback_id);
            if (cb)
            {
                ffslot_req_arg arg(msg_.body, msg_.from_node_id, msg_.callback_id, msg_.bridge_route_id, this);
                cb->exe(&arg);
                m_ffslot_callback.del(msg_.callback_id);
                return 0;
            }
            else
            {
                LOGERROR((FFRPC, "ffrpc_t::handle_broker_route_msg callback_id[%u] not found", msg_.callback_id));
            }
        }
        else//! 表示调用接口
        {
            ffslot_t::callback_t* cb = m_ffslot_interface.get_callback(msg_.msg_id);
            if (cb)
            {
                ffslot_req_arg arg(msg_.body, msg_.from_node_id, msg_.callback_id, msg_.bridge_route_id, this);
                cb->exe(&arg);
                LOGINFO((FFRPC, "ffrpc_t::handle_broker_route_msg end ok msg_.bridge_route_id=%u", msg_.bridge_route_id));
                return 0;
            }
            else
            {
                LOGERROR((FFRPC, "ffrpc_t::handle_broker_route_msg msg_id[%u] not found", msg_.msg_id));
            }
        }
    }
    catch(exception& e_)
    {
        LOGERROR((BROKER, "ffbroker_t::handle_broker_route_msg exception<%s>", e_.what()));
        if (msg_.msg_id == 0)//! callback msg
        {
            m_ffslot.del(msg_.callback_id);
        }
        return -1;
    }
    LOGTRACE((FFRPC, "ffrpc_t::handle_broker_route_msg end"));
    return 0;
}

int ffrpc_t::call_impl(const string& service_name_, const string& msg_name_, const string& body_, ffslot_t::callback_t* callback_)
{
    //!调用远程消息
    LOGTRACE((FFRPC, "ffrpc_t::call_impl begin service_name_<%s>, msg_name_<%s> body_size=%u", service_name_.c_str(), msg_name_.c_str(), body_.size()));
    int64_t callback_id  = int64_t(callback_);
    m_ffslot_callback.bind(callback_id, callback_);
    
    broker_route_msg_t::in_t dest_msg;
    dest_msg.dest_service_name = service_name_;
    dest_msg.dest_msg_name = msg_name_;
    dest_msg.dest_node_id  = m_broker_data.service2node_id[service_name_];
    dest_msg.from_node_id  = m_node_id;
    dest_msg.callback_id = callback_id;
    dest_msg.body = body_;
    msg_sender_t::send(get_broker_socket(), BROKER_ROUTE_MSG, dest_msg);
    LOGTRACE((FFRPC, "ffrpc_t::call_impl end dest_id=%u", dest_msg.dest_node_id));
    /*
    map<string, uint32_t>::iterator it = m_broker_client_name2nodeid.find(service_name_);
    if (it == m_broker_client_name2nodeid.end())
    {
        delete callback_;
        return -1;
    }

    uint32_t dest_node_id = it->second;
    uint32_t msg_id       = 0;
    map<string, uint32_t>::iterator msg_it = m_msg2id.find(msg_name_);
    if (msg_it == m_msg2id.end())
    {
        delete callback_;
        LOGERROR((FFRPC, "ffrpc_t::call_impl begin service_name_<%s>, no msg_name_<%s> registed", service_name_.c_str(), msg_name_.c_str()));
        return -1;
    }
    msg_id = msg_it->second;
    
    uint32_t callback_id  = 0;

    if (callback_)
    {
        callback_id = get_callback_id();
        m_ffslot_callback.bind(callback_id, callback_);
    }

    send_to_broker_by_nodeid(dest_node_id, body_, msg_id, callback_id);
    */
    LOGTRACE((FFRPC, "ffrpc_t::call_impl callback_id[%u] end ok", callback_id));
    return 0;
}
//! 判断某个service是否存在
bool ffrpc_t::is_exist(const string& service_name_)
{
    map<string, uint32_t>::iterator it = m_broker_client_name2nodeid.find(service_name_);
    if (it == m_broker_client_name2nodeid.end())
    {
        return false;
    }
    return true;
}
//! 通过bridge broker调用远程的service
int ffrpc_t::bridge_call_impl(const string& broker_group_, const string& service_name_, const string& msg_name_,
                              const string& body_, ffslot_t::callback_t* callback_)
{
    broker_route_to_bridge_t::in_t dest_msg;
    dest_msg.dest_broker_group_name = broker_group_;
    dest_msg.service_name           = service_name_;//!  服务名
    dest_msg.msg_name               = msg_name_;//!消息名
    dest_msg.body                   = body_;//! msg data
    dest_msg.from_node_id           = m_node_id;
    dest_msg.dest_node_id           = 0;
    dest_msg.callback_id            = 0;
    if (callback_)
    {
        dest_msg.callback_id = get_callback_id();
        m_ffslot_callback.bind(dest_msg.callback_id, callback_);
    }

    msg_sender_t::send(get_broker_socket(), BROKER_TO_BRIDGE_ROUTE_MSG, dest_msg);
    LOGINFO((FFRPC, "ffrpc_t::bridge_call_impl group<%s> service[%s] end ok", broker_group_, service_name_));
    return 0;
}
//! 通过node id 发送消息给broker
void ffrpc_t::send_to_broker_by_nodeid(uint32_t dest_node_id, const string& body_, uint32_t msg_id_, uint32_t callback_id_, uint32_t bridge_route_id_)
{
    LOGINFO((FFRPC, "ffrpc_t::send_to_broker_by_nodeid begin dest_node_id[%u]", dest_node_id));
    broker_route_msg_t::in_t dest_msg;
    //dest_msg.dest_service_name = service_name_;
    //dest_msg.dest_msg_name = msg_name_;
    dest_msg.dest_node_id = dest_node_id;
    dest_msg.callback_id = callback_id_;
    dest_msg.body = body_;
    msg_sender_t::send(get_broker_socket(), BROKER_ROUTE_MSG, dest_msg);
    return;
    
    broker_route_t::in_t msg;
    msg.dest_node_id     = dest_node_id;
    msg.from_node_id     = m_node_id;
    msg.msg_id           = msg_id_;
    msg.body             = body_;
    msg.callback_id      = callback_id_;
    msg.bridge_route_id  = bridge_route_id_;

    uint32_t broker_node_id = BROKER_MASTER_NODE_ID;
    
    if (bridge_route_id_ != 0)
    {
        //! 需要转发给bridge broker标记，若此值不为0，说明目标node在其他broker组
        //! 需要broker master转发到bridge broker上
        broker_node_id = BROKER_MASTER_NODE_ID;
    }
    //!  如果是response 消息，那么从哪个broker来，再从哪个broker 回去
    else if (msg_id_ == 0)
    {
        broker_node_id = m_broker_client_info[m_node_id].bind_broker_id;
    }
    else//! call消息，需要找到目标节点绑定的broker
    {
        broker_client_info_t& broker_client_info = m_broker_client_info[dest_node_id];
        broker_node_id = broker_client_info.bind_broker_id;
    }
    //!如果在同一个进程内那么，内存转发
    if (false && 0 == singleton_t<ffrpc_memory_route_t>::instance().client_route_to_broker(broker_node_id, msg))
    {
        LOGTRACE((FFRPC, "ffrpc_t::send_to_broker_by_nodeid dest_node_id[%u], broker_node_id[%u], msgid<%u>, callback_id_[%u] same process",
                        dest_node_id, broker_node_id, msg_id_, callback_id_));
        return;
    }

    if (broker_node_id == BROKER_MASTER_NODE_ID)
    {
        msg_sender_t::send(get_broker_socket(), BROKER_ROUTE_MSG, msg);
    }
    else
    {
        map<uint32_t, slave_broker_info_t>::iterator it_slave_broker = m_slave_broker_sockets.find(broker_node_id);
        if (it_slave_broker != m_slave_broker_sockets.end())
        {
            msg_sender_t::send(it_slave_broker->second.sock, BROKER_ROUTE_MSG, msg);
        }
    }
    LOGTRACE((FFRPC, "ffrpc_t::send_to_broker_by_nodeid dest_node_id[%u], broker_node_id[%u], msgid<%u>, callback_id_[%u] end ok",
                        dest_node_id, broker_node_id, msg_id_, callback_id_));
}

//! 调用接口后，需要回调消息给请求者
void ffrpc_t::response(uint32_t node_id_, uint32_t msg_id_, uint32_t callback_id_, uint32_t bridge_route_id_, const string& body_)
{
    m_tq.produce(task_binder_t::gen(&ffrpc_t::send_to_broker_by_nodeid, this, node_id_, body_, msg_id_, callback_id_, bridge_route_id_));
}

//! 处理注册, 
int ffrpc_t::handle_broker_reg_response(register_to_broker_t::out_t& msg_, socket_ptr_t sock_)
{
    LOGTRACE((FFRPC, "ffbroker_t::handle_broker_reg_response begin node_id=%d", msg_.node_id));
    if (msg_.register_flag < 0)
    {
        LOGERROR((FFRPC, "ffbroker_t::handle_broker_reg_response register failed, service exist"));
        return -1;
    }
    if (msg_.register_flag == 1)
    {
        m_node_id = msg_.node_id;//! -1表示注册失败，0表示同步消息，1表示注册成功
        m_bind_broker_id = msg_.bind_broker_id;
    }
    m_broker_data = msg_;
    
    timer_reconnect_broker();
    LOGTRACE((FFRPC, "ffbroker_t::handle_broker_reg_response end service num=%d", m_broker_data.service2node_id.size()));
    return 0;
}

//!获取broker socket
socket_ptr_t ffrpc_t::get_broker_socket()
{
    if (m_bind_broker_id == 0)
    {
        return m_master_broker_sock;
    }
    return m_broker_sockets[m_bind_broker_id];     
}
