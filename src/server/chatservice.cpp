#include "chatservice.hpp"
#include "public.hpp"
#include <string>
#include <vector>
#include <muduo/base/Logging.h>
using namespace muduo;
using namespace std;

// 获取单例对象的接口函数
ChatService* ChatService::instance()
{
    static ChatService service;
    return &service;
}

// 注册消息以及对应的回调函数
ChatService::ChatService()
{
    _msgHandlerMap.insert({LOGIN_MSG, std::bind(&ChatService::login, this, _1, _2, _3)});
    _msgHandlerMap.insert({REG_MSG, std::bind(&ChatService::reg, this, _1, _2, _3)});
    _msgHandlerMap.insert({ONE_CHAT_MSG, std::bind(&ChatService::oneChat, this, _1, _2, _3)});
    _msgHandlerMap.insert({ADD_FRIEND_MSG, std::bind(&ChatService::addFriend, this, _1, _2, _3)});
    _msgHandlerMap.insert({CREATE_GROUP_MSG, std::bind(&ChatService::createGroup, this, _1, _2, _3)});
    _msgHandlerMap.insert({ADD_GROUP_MSG, std::bind(&ChatService::addGroup, this, _1, _2, _3)});
    _msgHandlerMap.insert({GROUP_CHAT_MSG, std::bind(&ChatService::groupChat, this, _1, _2, _3)});
    _msgHandlerMap.insert({LOGINOUT_MSG, std::bind(&ChatService::loginout, this, _1, _2, _3)});

    // 连接redis服务器
    if (_redis.connect()) {
        // 设置上报消息的回调
        _redis.init_notify_handler(std::bind(&ChatService::handleRedisSubscribeMessage, this, _1, _2));
    }

}

// 处理服务器异常，重置业务
void ChatService::reset()
{
    _userModel.resetState();
}

// 获取消息对应的处理器
MsgHandler ChatService::getHandler(int msgid)
{
    // 记录错误日志，msgid没有对应的事件处理回调
    auto it = _msgHandlerMap.find(msgid);
    if (it == _msgHandlerMap.end()) {
        // 返回一个默认的处理器
        return [=](const TcpConnectionPtr &conn, json &js, Timestamp time) {
            LOG_ERROR << "msgid: " << msgid << " can not find handler!";
        };
    }
    else
        return _msgHandlerMap[msgid];
}

// 处理登录业务 id pwd
void ChatService::login(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int id = js["id"];
    string pwd = js["password"];

    User user = _userModel.query(id);
    if (user.getId() == id && user.getPwd() == pwd) {
        if (user.getState() == "online") {
            // 用户已经登录
            json response;
            response["msgid"] = LOGIN_MSG_ACK;
            response["errno"] = 2;
            response["errmsg"] = "this accout is online, don't login again!";
            conn->send(response.dump());
        }
        else {
            // 登录成功
            
            // 记录用户连接信息
            { // 将线程安全的代码放进作用域之中，出了作用域就自动释放锁了！！！
                // C++11提供的多线程相关操作
                lock_guard<mutex> lock(_connMutex);
                _userConnMap.insert({user.getId(), conn});
            }

            // 登录成功后，向redis订阅channel(id)
            _redis.subscribe(id);

            // 更新用户状态信息
            user.setState("online");
            _userModel.updateState(user);

            json response;
            response["msgid"] = LOGIN_MSG_ACK;
            response["errno"] = 0;
            response["id"] = user.getId();
            response["name"] = user.getName();
            // 查询用户是否有离线消息
            vector<string> vec = _offlineMsgModel.query(id);
            if (!vec.empty())
            {
                response["offlinemsg"] = vec;
                // 将用户的离线消息删除
                _offlineMsgModel.remove(id);
            }
            // 查询用户的好友信息并返回
            vector<User> friendVec = _friendModel.query(id);
            if (!friendVec.empty())
            {
                vector<string> friends;
                for (auto &fri : friendVec) {
                    json js;
                    js["id"] = fri.getId();
                    js["name"] = fri.getName();
                    js["state"] = fri.getState();
                    friends.push_back(js.dump());
                }
                response["friends"] = friends;
            }
            // 查询用户的群组信息
            vector<Group> groupVec = _groupModel.queryGroups(id);
            if (!groupVec.empty()) {
                vector<string> groups;
                for (Group &grp : groupVec) {
                    json grpjson;
                    grpjson["id"] = grp.getId();
                    grpjson["groupname"] = grp.getName();
                    grpjson["groupdesc"] = grp.getDesc();
                    vector<string> grpusers;
                    for (GroupUser &grpuser : grp.getUsers()) {
                        json grpuserjson;
                        grpuserjson["id"] = grpuser.getId();
                        grpuserjson["name"] = grpuser.getName();
                        grpuserjson["state"] = grpuser.getState();
                        grpuserjson["role"] = grpuser.getRole();
                        grpusers.push_back(grpuserjson.dump());
                    }
                    grpjson["users"] = grpusers;
                    groups.push_back(grpjson.dump());
                }
                response["groups"] = groups;
            }
            conn->send(response.dump());
        }
        
    }
    else {
        // 登录失败
        json response;
        response["msgid"] = LOGIN_MSG_ACK;
        response["errno"] = 1;
        response["errmsg"] = "wrong userid or password!";
        conn->send(response.dump());
    }

}

// 处理注册业务 name password
void ChatService::reg(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    string name = js["name"];
    string pwd = js["password"];

    User user;
    user.setName(name);
    user.setPwd(pwd);
    if (_userModel.insert(user))
    {
        // 注册成功
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] = 0;
        response["id"] = user.getId();
        conn->send(response.dump());
    }
    else
    {
        // 注册失败
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] = 1;
        conn->send(response.dump());
    }
}

// 处理注销业务
void ChatService::loginout(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"];

    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(userid);
        if (it != _userConnMap.end()) {
            _userConnMap.erase(it);
        }
    }

    // 用户注销，在redis中取消订阅通道
    _redis.unsubscribe(userid);

    // 更新用户的状态信息
    User user(userid, "", "", "offline");
    _userModel.updateState(user);
}

// 处理客户端异常退出
void ChatService::clientCloseException(const TcpConnectionPtr &conn)
{
    User user;
    {
        lock_guard<mutex> lock(_connMutex);
        for (auto it = _userConnMap.begin(); it != _userConnMap.end(); it++) {
            if (it->second == conn) {
                user.setId(it->first); // 记录当前用户的id
                _userConnMap.erase(it); // 将当前用户的连接删除
                break;
            }
        }
    }
    
    // 用户注销，在redis中取消订阅通道
    _redis.unsubscribe(user.getId());

    // 更新用户的状态
    if (user.getId() != -1) {
        user.setState("offline");
        _userModel.updateState(user);
    }

}

// 处理一对一聊天业务
void ChatService::oneChat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int toid = js["toid"];
    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(toid);
        if (it != _userConnMap.end()) {
            // toid在线，转发消息
            it->second->send(js.dump());
            return;
        }
    }

    // 查询toid是否在线
    User user = _userModel.query(toid);
    if (user.getState() == "online") {
        // 说明在线，但不在同一个服务器上
        _redis.publish(toid, js.dump());
        return;
    }

    // toid不在线，存储离线消息
    _offlineMsgModel.insert(toid, js.dump());
}

// 添加好友业务
void ChatService::addFriend(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"];
    int friendid = js["friendid"];

    // 存储好友信息
    _friendModel.insert(userid, friendid);
}

// 创建群组业务
void ChatService::createGroup(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"];
    string groupName = js["groupname"];
    string groupDesc = js["groupdesc"];

    Group group(-1, groupName, groupDesc);
    if (_groupModel.createGroup(group)) {
        // 存入群组创建人的信息
        _groupModel.addGroup(userid, group.getId(), "creator");
    }
}

// 加入群组业务
void ChatService::addGroup(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"];
    int groupid = js["groupid"];
    _groupModel.addGroup(userid, groupid, "normal");
}

// 群组聊天业务
void ChatService::groupChat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"];
    int groupid = js["groupid"];
    vector<int> useridVec = _groupModel.queryGroupUsers(userid, groupid);

    lock_guard<mutex> lock(_connMutex);
    for (int id : useridVec) {
        auto it = _userConnMap.find(id);
        if (it != _userConnMap.end()) {
            // 转发群消息
            it->second->send(js.dump());
        }
        else {

            User user = _userModel.query(id);
            if (user.getState() == "online") {
                _redis.publish(id, js.dump());
            }
            else {
                // 存储离线群消息
                _offlineMsgModel.insert(id, js.dump());
            }
        }
    }
}

// 从redis消息队列中获取订阅的消息
void ChatService::handleRedisSubscribeMessage(int userid, string msg)
{
    //json js = json::parse(msg.c_str());

    lock_guard<mutex> lock(_connMutex);
    auto it = _userConnMap.find(userid);
    if (it != _userConnMap.end()) {
        it->second->send(msg);
        return;
    }

    // 存储该用户的离线消息
    _offlineMsgModel.insert(userid, msg);
}