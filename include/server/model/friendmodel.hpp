#ifndef FRIENDMODEL_H
#define FRIENDMODEL_H

#include "user.hpp"
#include <vector>
using namespace std;
class FriendModel
{
public:
    // 添加好友关系
    void insert(int userid, int friendid);

    // 查询用户好友列表（可以存在本地减少服务器压力）
    vector<User> query(int userid);

};

#endif