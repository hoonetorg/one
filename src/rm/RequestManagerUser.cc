/* -------------------------------------------------------------------------- */
/* Copyright 2002-2016, OpenNebula Project, OpenNebula Systems                */
/*                                                                            */
/* Licensed under the Apache License, Version 2.0 (the "License"); you may    */
/* not use this file except in compliance with the License. You may obtain    */
/* a copy of the License at                                                   */
/*                                                                            */
/* http://www.apache.org/licenses/LICENSE-2.0                                 */
/*                                                                            */
/* Unless required by applicable law or agreed to in writing, software        */
/* distributed under the License is distributed on an "AS IS" BASIS,          */
/* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   */
/* See the License for the specific language governing permissions and        */
/* limitations under the License.                                             */
/* -------------------------------------------------------------------------- */

#include "RequestManagerUser.h"

using namespace std;

void RequestManagerUser::
    request_execute(xmlrpc_c::paramList const& paramList,
                    RequestAttributes& att)
{
    int    id  = xmlrpc_c::value_int(paramList.getInt(1));
    User * user;
    string error_str;

    if ( basic_authorization(id, att) == false )
    {
        return;
    }

    user = static_cast<User *>(pool->get(id,false));

    if ( user == 0 )
    {
        att.resp_id = id;
        failure_response(NO_EXISTS, att);
        return;
    }

    if ( user_action(id, paramList, att, att.resp_msg) < 0 )
    {
        failure_response(ACTION, att);
        return;
    }

    success_response(id, att);
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int UserChangePassword::user_action(int     user_id,
                                    xmlrpc_c::paramList const& paramList,
                                    RequestAttributes&         att,
                                    string& error_str)
{

    string new_pass = xmlrpc_c::value_string(paramList.getString(2));
    User * user;

    string driver;
    bool   allowed = false;
    const VectorAttribute* auth_conf;

    user = static_cast<User *>(pool->get(user_id,true));

    if ( user == 0 )
    {
        return -1;
    }

    driver = user->get_auth_driver();

    if (Nebula::instance().get_auth_conf_attribute(driver, auth_conf) == 0)
    {
        auth_conf->vector_value("PASSWORD_CHANGE", allowed);
    }

    if (!allowed &&
        att.uid != UserPool::ONEADMIN_ID &&
        att.gid != GroupPool::ONEADMIN_ID)
    {
        error_str = "Password for driver '"+user->get_auth_driver()+
                    "' cannot be changed.";

        user->unlock();
        return -1;
    }

    int rc = user->set_password(new_pass, error_str);

    if ( rc == 0 )
    {
        pool->update(user);
    }

    user->unlock();

    return rc;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int UserChangeAuth::user_action(int     user_id,
                                xmlrpc_c::paramList const& paramList,
                                RequestAttributes&         att,
                                string& error_str)
{
    string new_auth = xmlrpc_c::value_string(paramList.getString(2));
    string new_pass = xmlrpc_c::value_string(paramList.getString(3));

    int    rc = 0;

    User * user;

    user = static_cast<User *>(pool->get(user_id,true));

    if ( user == 0 )
    {
        return -1;
    }

    string old_auth = user->get_auth_driver();

    rc = user->set_auth_driver(new_auth, error_str);

    if ( rc == 0 && !new_pass.empty() )
    {
        rc = user->set_password(new_pass, error_str);

        if (rc != 0)
        {
            string tmp_str;

            user->set_auth_driver(old_auth, tmp_str);
        }
    }

    if ( rc == 0 )
    {
        pool->update(user);
    }

    user->unlock();

    return rc;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int UserSetQuota::user_action(int     user_id,
                              xmlrpc_c::paramList const& paramList,
                              RequestAttributes&         att,
                              string& error_str)
{

    string   quota_str = xmlrpc_c::value_string(paramList.getString(2));
    Template quota_tmpl;

    int    rc;
    User * user;

    if ( user_id == UserPool::ONEADMIN_ID )
    {
        error_str = "Cannot set quotas for oneadmin user";
        return -1;
    }

    rc = quota_tmpl.parse_str_or_xml(quota_str, error_str);

    if ( rc != 0 )
    {
        return -1;
    }

    user = static_cast<User *>(pool->get(user_id,true));

    if ( user == 0 )
    {
        return -1;
    }

    rc = user->quota.set(&quota_tmpl, error_str);

    static_cast<UserPool *>(pool)->update_quotas(user);

    user->unlock();

    return rc;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void UserEditGroup::
    request_execute(xmlrpc_c::paramList const& paramList,
                    RequestAttributes& att)
{
    int user_id  = xmlrpc_c::value_int(paramList.getInt(1));
    int group_id = xmlrpc_c::value_int(paramList.getInt(2));

    int rc;

    string error_str;

    string gname;
    string uname;
    string auth_driver;

    PoolObjectAuth uperms;
    PoolObjectAuth gperms;

    const VectorAttribute* auth_conf;
    bool driver_managed_groups;

    User* user;

    if ((user = upool->get(user_id,true)) == 0 )
    {
        att.resp_obj = PoolObjectSQL::USER;
        att.resp_id  = user_id;
        failure_response(NO_EXISTS, att);

        return;
    }

    user->get_permissions(uperms);

    uname = user->get_name();

    auth_driver = user->get_auth_driver();

    user->unlock();

    driver_managed_groups = false;

    if (Nebula::instance().get_auth_conf_attribute(auth_driver, auth_conf) == 0)
    {
        auth_conf->vector_value("DRIVER_MANAGED_GROUPS", driver_managed_groups);
    }

    if (driver_managed_groups)
    {
        att.resp_msg =
            "Groups cannot be manually managed for auth driver "+auth_driver;
        failure_response(ACTION, att);
        return;
    }

    rc = get_info(gpool,group_id,PoolObjectSQL::GROUP,att,gperms,gname,true);

    if ( rc == -1 )
    {
        return;
    }

    if ( att.uid != UserPool::ONEADMIN_ID )
    {
        AuthRequest ar(att.uid, att.group_ids);

        ar.add_auth(AuthRequest::MANAGE, uperms);   // MANAGE USER
        ar.add_auth(AuthRequest::MANAGE, gperms);   // MANAGE GROUP

        if (UserPool::authorize(ar) == -1)
        {
            att.resp_msg = ar.message;
            failure_response(AUTHORIZATION, att);
            return;
        }
    }

    if ( secondary_group_action(user_id, group_id, paramList, att.resp_msg) < 0 )
    {
        failure_response(ACTION, att);
        return;
    }

    success_response(user_id, att);
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int UserAddGroup::secondary_group_action(
            int                        user_id,
            int                        group_id,
            xmlrpc_c::paramList const& _paramList,
            string&                    error_str)
{
    User *  user;
    Group * group;

    int rc;

    user = upool->get(user_id,true);

    if ( user == 0 )
    {
        return -1;
    }

    rc = user->add_group(group_id);

    if ( rc != 0 )
    {
        user->unlock();

        error_str = "User is already in this group";
        return -1;
    }

    upool->update(user);

    user->unlock();

    group = gpool->get(group_id, true);

    if( group == 0 )
    {
        user = upool->get(user_id,true);

        if ( user != 0 )
        {
            user->del_group(group_id);

            upool->update(user);

            user->unlock();
        }

        error_str = "Group does not exist";
        return -1;
    }

    group->add_user(user_id);

    gpool->update(group);

    group->unlock();

    return 0;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int UserDelGroup::secondary_group_action(
            int                        user_id,
            int                        group_id,
            xmlrpc_c::paramList const& _paramList,
            string&                    error_str)
{
    User *  user;
    Group * group;

    int rc;

    user = upool->get(user_id,true);

    rc = user->del_group(group_id);

    if ( rc != 0 )
    {
        user->unlock();

        if ( rc == -1 )
        {
            error_str = "User is not part of this group";
        }
        else if ( rc == -2 )
        {
            error_str = "Cannot remove user from the primary group";
        }
        else
        {
            error_str = "Cannot remove user from group";
        }

        return rc;
    }

    upool->update(user);

    user->unlock();

    group = gpool->get(group_id, true);

    if( group == 0 )
    {
        //Group does not exist, should never occur
        error_str = "Cannot remove user from group";
        return -1;
    }

    group->del_user(user_id);

    gpool->update(group);

    group->unlock();

    return 0;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void UserLogin::request_execute(xmlrpc_c::paramList const& paramList,
                    RequestAttributes& att)
{
    string uname = xmlrpc_c::value_string(paramList.getString(1));
    string token = xmlrpc_c::value_string(paramList.getString(2));
    time_t valid = xmlrpc_c::value_int(paramList.getInt(3));

    User * user;
    string error_str;
    string auth_driver;
    time_t max_token_time;
    const VectorAttribute* auth_conf;

    PoolObjectAuth perms;

    if (att.uid != 0)
    {
        user = static_cast<UserPool *>(pool)->get(uname,true);

        if ( user == 0 )
        {
            failure_response(NO_EXISTS, att);
            return;
        }

        user->get_permissions(perms);

        user->unlock();


        AuthRequest ar(att.uid, att.group_ids);

        ar.add_auth(auth_op, perms);

        if (UserPool::authorize(ar) == -1)
        {
            att.resp_msg = ar.message;
            failure_response(AUTHORIZATION, att);
            return;
        }
    }

    user = static_cast<UserPool *>(pool)->get(uname,true);

    if ( user == 0 )
    {
        failure_response(NO_EXISTS, att);
        return;
    }

    auth_driver = user->get_auth_driver();
    max_token_time = -1;

    if (Nebula::instance().get_auth_conf_attribute(auth_driver, auth_conf) == 0)
    {
        auth_conf->vector_value("MAX_TOKEN_TIME", max_token_time);
    }

    if (max_token_time == 0)
    {
        att.resp_msg = "Login tokens are disabled for driver '"+
                        user->get_auth_driver()+"'";
        failure_response(ACTION,  att);

        // Reset token
        user->login_token.reset();
        pool->update(user);
        user->unlock();

        return;
    }
    else if (max_token_time > 0)
    {
        valid = max(valid, max_token_time);

        if (max_token_time < valid)
        {
            valid = max_token_time;

            ostringstream oss;

            oss << "Req:" << att.req_id << " " << method_name
                << " Token time has been overwritten with the MAX_TOKEN_TIME of "
                << max_token_time << " set in oned.conf";
            NebulaLog::log("ReM",Log::WARNING,oss);
        }
    }

    if (valid == 0) //Reset token
    {
        user->login_token.reset();

        token = "";
    }
    else if (valid > 0 || valid == -1)
    {
        token = user->login_token.set(token, valid);
    }
    else
    {
        att.resp_msg = "Wrong valid period for token";
        failure_response(XML_RPC_API,  att);

        user->unlock();
        return;
    }

    pool->update(user);

    user->unlock();

    success_response(token, att);
}
