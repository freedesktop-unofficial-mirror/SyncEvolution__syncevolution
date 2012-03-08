/*
 * Copyright(C) 2012 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or(at your option) version 3.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

#include "server.h"
#include "client.h"
#include "connection-resource.h"
#include "session-common.h"
#include "dbus-proxy.h"
#include "dbus-callbacks.h"

#include <synthesis/san.h>
#include <syncevo/TransportAgent.h>
#include <syncevo/SyncContext.h>

#include <boost/lexical_cast.hpp>
#include <boost/bind.hpp>

#include <memory>

using namespace GDBusCXX;

SE_BEGIN_CXX

std::string ConnectionResource::buildDescription(const StringMap &peer)
{
    StringMap::const_iterator
        desc = peer.find("description"),
        id = peer.find("id"),
        trans = peer.find("transport"),
        trans_desc = peer.find("transport_description");
    std::string buffer;
    buffer.reserve(256);
    if (desc != peer.end()) {
        buffer += desc->second;
    }
    if (id != peer.end() || trans != peer.end()) {
        if (!buffer.empty()) {
            buffer += " ";
        }
        buffer += "(";
        if (id != peer.end()) {
            buffer += id->second;
            if (trans != peer.end()) {
                buffer += " via ";
            }
        }
        if (trans != peer.end()) {
            buffer += trans->second;
            if (trans_desc != peer.end()) {
                buffer += " ";
                buffer += trans_desc->second;
            }
        }
        buffer += ")";
    }
    return buffer;
}

void ConnectionResource::process(const Caller_t &caller,
                                 const GDBusCXX::DBusArray<uint8_t> &msg,
                                 const std::string &msgType,
                                 const boost::shared_ptr<GDBusCXX::Result0> &result)
{
    boost::shared_ptr<Client> client(m_server.findClient(caller));
    if (!client) {
        throw runtime_error("unknown client");
    }

    boost::shared_ptr<ConnectionResource> myself =
        boost::static_pointer_cast<ConnectionResource, Resource>(client->findResource(this));
    if (!myself) {
        throw runtime_error("client does not own connection");
    }

    ProxyCallback0 callback(result);

    defaultConnectToBoth(callback, m_connectionProxy->m_process.getMethod());
    m_connectionProxy->m_process.start(msg,
                                       msgType,
                                       m_peer,
                                       m_mustAuthenticate,
                                       callback);
}

void ConnectionResource::close(const GDBusCXX::Caller_t &caller, bool normal, const std::string &error,
                               const boost::shared_ptr<GDBusCXX::Result0> &result)
{
    SE_LOG_DEBUG(NULL, NULL, "D-Bus client %s closes connection %s %s%s%s",
                 caller.c_str(),
                 getPath(),
                 normal ? "normally" : "with error",
                 error.empty() ? "" : ": ",
                 error.c_str());

    boost::shared_ptr<Client> client(m_server.findClient(caller));
    if (!client) {
        throw runtime_error("unknown client");
    }

    // Does the client have to own this resource to close it?
    // Probably. We also need the smart pointer below.
    boost::shared_ptr<ConnectionResource> myself =
        boost::static_pointer_cast<ConnectionResource, Resource>(client->findResource(this));
    if (!myself) {
        throw runtime_error("client does not own connection");
    }

    // If the close() call succeeds, then we remove ourselves from
    // the client. boost::signals2 tracking ensures that
    // Client::detach() will not be called with a stale Client or
    // ConnectionResource pointer.
    //
    // static_cast is necessary because detach() is ambiguous.
    ProxyCallback0 callback(result);

    callback.m_success->connect(ProxyCallback0::SuccessSignalType::slot_type(static_cast< void (Client::*) (Resource *)>(&Client::detach),
                                                                             client.get(),
                                                                             myself.get()).track(client).track(myself));
    defaultConnectToFailure(callback, m_connectionProxy->m_close.getMethod());
    m_connectionProxy->m_close.start(normal,
                                     error,
                                     callback);
}

void ConnectionResource::replyCb(const GDBusCXX::DBusArray<uint8_t> &reply, const std::string &replyType,
                                 const StringMap &meta, bool final, const std::string &session)
{
    emitReply(reply, replyType, meta, final, session);
    SE_LOG_INFO(NULL, NULL, "Connection.Reply signal received: replyType=%s, final=%s, session=%s",
                replyType.c_str(), final ? "T" : "F", session.c_str());
}

void ConnectionResource::shutdownCb()
{
    SE_LOG_INFO(NULL, NULL, "Connection.Shutdown signal received: detaching connection from server.");
    m_server.detach(this);
}

void ConnectionResource::killSessionsCb(const string &peerDeviceId)
{
    SE_LOG_INFO(NULL, NULL, "Connection.KillSessions signal received: peerDeviceId=%s.", peerDeviceId.c_str());
    m_server.killSessions(peerDeviceId, boost::function<void()>(&NullCb));
}

void ConnectionResource::abortCb()
{
    SE_LOG_INFO(NULL, NULL, "Connection.Abort signal received");

    if(!m_abortSent) {
        emitAbort();
        m_abortSent = true;
    }
}

void ConnectionResource::init(const Callback_t &callback)
{
    SE_LOG_INFO(NULL, NULL, "ConnectionResource (%s) forking...", getPath());

    m_forkExecParent->m_onConnect.connect(boost::bind(&ConnectionResource::onConnect, this, _1));
    m_forkExecParent->m_onReady.connect(boost::bind(&ConnectionResource::onReady, this, callback));
    m_forkExecParent->m_onQuit.connect(boost::bind(&ConnectionResource::onQuit, this, _1));
    m_forkExecParent->m_onFailure.connect(boost::bind(&ConnectionResource::onFailure, this, _2));
    m_forkExecParent->addEnvVar("SYNCEVO_START_CONNECTION", "TRUE");
    m_forkExecParent->addEnvVar("SYNCEVO_SESSION_ID", m_sessionID);
    m_forkExecParent->start();
}

void ConnectionResource::onConnect(const GDBusCXX::DBusConnectionPtr &conn)
{
    m_helper_conn = conn;
}

void ConnectionResource::onReady(const Callback_t &callback)
{
    SE_LOG_INFO(NULL, NULL, "ConnectionProxy interface ending with: %s", m_sessionID.c_str());
    m_connectionProxy.reset(new ConnectionProxy(m_helper_conn, m_sessionID));

    /* Enable public dbus interface for Connection. */
    activate();

    // Activate signal watch on helper signals.
    m_connectionProxy->m_reply.activate       (boost::bind(&ConnectionResource::replyCb,
                                                           this, _1, _2, _3 ,_4, _5));
    m_connectionProxy->m_abort.activate       (boost::bind(&ConnectionResource::abortCb, this));
    m_connectionProxy->m_shutdown.activate    (boost::bind(&ConnectionResource::shutdownCb, this));
    m_connectionProxy->m_killSessions.activate(boost::bind(&ConnectionResource::killSessionsCb, this, _1));

    SE_LOG_INFO(NULL, NULL, "onConnect called in ConnectionResource (path: %s interface: %s)",
                m_connectionProxy->getPath(), m_connectionProxy->getInterface());

    boost::shared_ptr<ConnectionResource> me(this);
    // if callback owner won't copy this shared pointer
    // then connection resource will be destroyed.
    callback(me);
}

void ConnectionResource::onQuit(int status)
{
    m_server.checkQueue(boost::function<void()>(&NullCb));
    SE_LOG_INFO(NULL, NULL, "dbus-helper quit with status: %d", status);
}

void ConnectionResource::onFailure(const std::string &error)
{
    m_server.checkQueue(boost::function<void()>(&NullCb));
    SE_LOG_INFO(NULL, NULL, "dbus-helper failed with error: %s", error.c_str());
}

void ConnectionResource::createConnectionResource(const Callback_t &callback,
                                                  Server &server,
                                                  const std::string &session_num,
                                                  const StringMap &peer,
                                                  bool must_authenticate)
{
    std::auto_ptr<ConnectionResource> resource(new ConnectionResource(server,
                                                                      session_num,
                                                                      peer,
                                                                      must_authenticate));

    resource->init(callback);
    // init did not throw any exception, so we guess that child was spawned successfully.
    // thus we release the auto_ptr, so it will not delete the resource.
    resource.release();
}

ConnectionResource::ConnectionResource(Server &server,
                                       const std::string &sessionID,
                                       const StringMap &peer,
                                       bool must_authenticate) :
    DBusObjectHelper(server.getConnection(),
                     std::string(SessionCommon::CONNECTION_PATH) + "/" + sessionID,
                     SessionCommon::CONNECTION_IFACE,
                     boost::bind(&Server::autoTermCallback, &server)),
    Resource(server, "Connection"),
    m_description(buildDescription(peer)),
    m_path(std::string(SessionCommon::CONNECTION_PATH) + "/" + sessionID),
    m_peer(peer),
    m_sessionID(sessionID),
    m_mustAuthenticate(must_authenticate),
    emitAbort(*this, "Abort"),
    m_abortSent(false),
    emitReply(*this, "Reply"),
    m_forkExecParent(SyncEvo::ForkExecParent::create("syncevo-dbus-helper"))
{
    m_priority = Resource::PRI_CONNECTION;
    // FIXME: A Connection is always marked as running for now as we
    // have no way of knowing the state of the session started by the
    // Connection.
    m_isRunning = true;

    add(this, &ConnectionResource::process, "Process");
    add(this, &ConnectionResource::close, "Close");
    add(emitAbort);
    add(emitReply);

    m_server.autoTermRef();
}

ConnectionResource::~ConnectionResource()
{
    m_server.autoTermUnref();
}

SE_END_CXX
