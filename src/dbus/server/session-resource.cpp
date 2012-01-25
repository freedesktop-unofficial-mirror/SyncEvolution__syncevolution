/*
 * Copyright (C) 2011 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) version 3.
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

#include "session-resource.h"
#include "client.h"
#include "restart.h"

#include <boost/foreach.hpp>

SE_BEGIN_CXX

void SessionResource::attach(const GDBusCXX::Caller_t &caller)
{
    boost::shared_ptr<Client> client(m_server.findClient(caller));
    if (!client) {
        throw runtime_error("unknown client");
    }
    boost::shared_ptr<SessionResource> me = m_me.lock();
    if (!me) {
        throw runtime_error("session resource already deleted?!");
    }
    client->attach(me);
}

void SessionResource::detach(const GDBusCXX::Caller_t &caller)
{
    boost::shared_ptr<Client> client(m_server.findClient(caller));
    if (!client) {
        throw runtime_error("unknown client");
    }
    client->detach(this);
}

void SessionResource::startShutdown()
{
    return;
}

void SessionResource::shutdownFileModified()
{
    return;
}

bool SessionResource::shutdownServer()
{
    Timespec now = Timespec::monotonic();
    bool autosync = m_server.getAutoSyncManager().hasTask() ||
        m_server.getAutoSyncManager().hasAutoConfigs();
    SE_LOG_DEBUG(NULL, NULL, "shut down server at %lu.%09lu because of file modifications, auto sync %s",
                 now.tv_sec, now.tv_nsec,
                 autosync ? "on" : "off");
    if (autosync) {
        // suitable exec() call which restarts the server using the same environment it was in
        // when it was started
        m_server.m_restart->restart();
    } else {
        // leave server now
        m_server.m_shutdownRequested = true;
        g_main_loop_quit(m_server.getLoop());
        SE_LOG_INFO(NULL, NULL, "server shutting down because files loaded into memory were modified on disk");
    }

    return false;
}

bool SessionResource::readyToRun()
{
    return true;
}

void SessionResource::restore(const string &dir, bool before, const std::vector<std::string> &sources)
{
    m_sessionProxy->m_restore(dir, before, sources, boost::bind(&SessionResource::restoreCb, this, _1));
}

void SessionResource::restoreCb(const std::string &error)
{
    if(!error.empty()) {
        m_result = false;
        SE_LOG_INFO(NULL, NULL, "Session.Restore callback returned: error=%s",
                    error.empty() ? "None" : error.c_str());
        return;
    }

    m_result = true;
    SE_LOG_INFO(NULL, NULL, "Session.Restore callback successfull");
}

void SessionResource::execute(const vector<string> &args, const map<string, string> &vars)
{
    m_sessionProxy->m_execute(args, vars, boost::bind(&SessionResource::executeCb, this, _1));
}

void SessionResource::executeCb(const std::string &error)
{
    if(!error.empty()) {
        m_result = false;
        SE_LOG_INFO(NULL, NULL, "Session.Execute callback returned: error=%s",
                    error.empty() ? "None" : error.c_str());
        return;
    }

    m_result = true;
    SE_LOG_INFO(NULL, NULL, "Session.Execute callback successfull");
}

bool SessionResource::getActive()
{
    return true;
}

void SessionResource::init()
{
    SE_LOG_INFO(NULL, NULL, "SessionResource (%s) forking...", getPath());

    m_forkExecParent->m_onConnect.connect(boost::bind(&SessionResource::onSessionConnect, this, _1));
    m_forkExecParent->m_onQuit.connect(boost::bind(&SessionResource::onQuit, this, _1));
    m_forkExecParent->m_onFailure.connect(boost::bind(&SessionResource::onFailure, this, _2));
    m_forkExecParent->addEnvVar("SYNCEVO_START_CONNECTION", "FALSE");
    m_forkExecParent->addEnvVar("SYNCEVO_SESSION_ID", m_sessionID);
    m_forkExecParent->start();

    // Wait for onSessionConnect to be called so that the dbus
    // interface is ready to be used.
    resetReplies();
    waitForReply();
}

void SessionResource::setNamedConfig(const std::string &configName, bool update, bool temporary,
                                     const ReadOperations::Config_t &config)
{
    // avoid the check if effect is the same as setConfig()
    if (m_configName != configName) {
        bool found = false;
        BOOST_FOREACH(const std::string &flag, m_flags) {
            if (boost::iequals(flag, "all-configs")) {
                found = true;
                break;
            }
        }
        if (!found) {
            SE_THROW_EXCEPTION(InvalidCall,
                               "SetNameConfig() only allowed in 'all-configs' sessions");
        }

        if (temporary) {
            SE_THROW_EXCEPTION(InvalidCall,
                               "SetNameConfig() with temporary config change only supported for config named when starting the session");
        }
    }

    m_server.getPresenceStatus().updateConfigPeers (configName, config);

    resetReplies();
    m_sessionProxy->m_setNamedConfig(configName, update, temporary, config,
                                     boost::bind(&SessionResource::setNamedConfigCb, this, _1, _2));
    waitForReply();
}

void SessionResource::setNamedConfigCb(bool setConfig, const std::string &error)
{
    if(!error.empty()) {
        m_result = false;
        SE_LOG_INFO(NULL, NULL, "Session.SetNamedConfig callback returned: error=%s",
                    error.empty() ? "None" : error.c_str());
        return;
    }

    m_setConfig = setConfig;
    m_result = true;
    SE_LOG_INFO(NULL, NULL, "Session.SetNamedConfig callback successfull: m_setConfig = %d", (int)setConfig);

    replyInc();
}

void SessionResource::sync(const std::string &mode, const SessionCommon::SourceModes_t &source_modes)
{
    m_sessionProxy->m_sync(mode, source_modes, boost::bind(&SessionResource::syncCb, this, _1));
}

void SessionResource::syncCb(const std::string &error)
{
    if(!error.empty()) {
        m_result = false;
        SE_LOG_INFO(NULL, NULL, "Session.Sync callback returned: error=%s",
                    error.empty() ? "None" : error.c_str());
        return;
    }

    m_result = true;
    SE_LOG_INFO(NULL, NULL, "Session.Sync callback successfull");
}

void SessionResource::abort()
{
    m_sessionProxy->m_abort(boost::bind(&SessionResource::abortCb, this, _1));
}

void SessionResource::abortCb(const string &error)
{
    if(!error.empty()) {
        m_result = false;
        SE_LOG_INFO(NULL, NULL, "Session.Abort callback returned: error=%s",
                    error.empty() ? "None" : error.c_str());
        return;
    }

    m_result = true;
    SE_LOG_INFO(NULL, NULL, "Session.Abort callback successfull");
}

void SessionResource::suspend()
{
    m_sessionProxy->m_suspend(boost::bind(&SessionResource::suspendCb, this, _1));
}

void SessionResource::suspendCb(const string &error)
{
    if(!error.empty()) {
        m_result = false;
        SE_LOG_INFO(NULL, NULL, "Session.Suspend callback returned: error=%s",
                    error.empty() ? "None" : error.c_str());
        return;
    }

    m_result = true;
    SE_LOG_INFO(NULL, NULL, "Session.Suspend callback successfull");
}

void SessionResource::getStatus(std::string &status, uint32_t &error,
                                SessionCommon::SourceStatuses_t &sources)
{
    resetReplies();
    m_sessionProxy->m_getStatus(boost::bind(&SessionResource::getStatusCb, this,
                                            &status, &error, &sources, _1, _2, _3, _4));
    waitForReply();
}

void SessionResource::getStatusCb(std::string *status, uint32_t *errorCode,
                                  SessionCommon::SourceStatuses_t *sources,
                                  const std::string &rStatus, uint32_t rErrorCode,
                                  const SessionCommon::SourceStatuses_t &rSources,
                                  const std::string &error)
{
    if(!error.empty()) {
        m_result = false;
        SE_LOG_INFO(NULL, NULL, "Session.GetStatus callback returned: error=%s",
                    error.empty() ? "None" : error.c_str());
    } else {
        m_result = true;
        *status    = rStatus;
        *errorCode = rErrorCode;
        *sources   = rSources;
        SE_LOG_INFO(NULL, NULL, "Session.GetStatus callback returned: status=%s, error code=%d",
                    status->c_str(), *errorCode);
    }

    replyInc();
}

void SessionResource::getProgress(int32_t &progress, SessionCommon::SourceProgresses_t &sources)
{
    resetReplies();
    m_sessionProxy->m_getProgress(boost::bind(&SessionResource::getProgressCb, this,
                                              &progress, &sources, _1, _2, _3));
    waitForReply();
}

void SessionResource::getProgressCb(int32_t *progress, SessionCommon::SourceProgresses_t *sources,
                                    int32_t rProgress, const SessionCommon::SourceProgresses_t &rSources,
                                    const std::string &error)
{
    if(!error.empty()) {
        m_result = false;
        SE_LOG_INFO(NULL, NULL, "Session.GetProgress callback returned: error=%s",
                    error.empty() ? "None" : error.c_str());
    } else {
        m_result  = true;
        *progress = rProgress;
        *sources  = rSources;
        SE_LOG_INFO(NULL, NULL, "Session.GetProgress callback returned: progess=%d", *progress);
    }

    replyInc();
}

void SessionResource::fireStatus(bool flush)
{
    std::string status;
    uint32_t error;
    SessionCommon::SourceStatuses_t sources;

    /** not force flushing and not timeout, return */
    if(!flush && !m_statusTimer.timeout()) {
        return;
    }
    m_statusTimer.reset();

    getStatus(status, error, sources);
    emitStatus(status, error, sources);
}

void SessionResource::fireProgress(bool flush)
{
    int32_t progress;
    SessionCommon::SourceProgresses_t sources;

    /** not force flushing and not timeout, return */
    if(!flush && !m_progressTimer.timeout()) {
        return;
    }
    m_progressTimer.reset();

    getProgress(progress, sources);
    emitProgress(progress, sources);
}

void SessionResource::getNamedConfig(const std::string &configName, bool getTemplate,
                                     ReadOperations::Config_t &config)
{
    resetReplies();
    m_sessionProxy->m_getNamedConfig(configName, getTemplate,
                                     boost::bind(&SessionResource::getNamedConfigCb, this, &config, _1, _2));
    waitForReply();
}

void SessionResource::getNamedConfigCb(ReadOperations::Config_t *config,
                                       const ReadOperations::Config_t &rConfig, const string &error)
{
    if(!error.empty()) {
        m_result = false;
        SE_LOG_INFO(NULL, NULL, "Session.GetNamedConfig callback returned: error=%s",
                    error.empty() ? "None" : error.c_str());
    } else {
        m_result = true;
        *config    = rConfig;
        SE_LOG_INFO(NULL, NULL, "Session.GetNamedConfig callback returned: success");
    }

    replyInc();
}

void SessionResource::getReports(uint32_t start, uint32_t count, ReadOperations::Reports_t &reports)
{
    resetReplies();
    m_sessionProxy->m_getReports(start, count,
                                 boost::bind(&SessionResource::getReportsCb, this, &reports, _1, _2));
    waitForReply();
}

void SessionResource::getReportsCb(ReadOperations::Reports_t *reports,
                                   const ReadOperations::Reports_t &rReports, const std::string &error)
{
    if(!error.empty()) {
        m_result = false;
        SE_LOG_INFO(NULL, NULL, "Session.GetReports callback returned: error=%s",
                    error.empty() ? "None" : error.c_str());
    } else {
        m_result = true;
        *reports    = rReports;
        SE_LOG_INFO(NULL, NULL, "Session.GetReports callback returned: success");
    }

    replyInc();
}

void SessionResource::checkSource(const string &sourceName)
{
    m_sessionProxy->m_checkSource(sourceName, boost::bind(&SessionResource::checkSourceCb, this, _1));
}

void SessionResource::checkSourceCb(const std::string &error)
{
    if(!error.empty()) {
        m_result = false;
        SE_LOG_INFO(NULL, NULL, "Session.CheckSource callback returned: error=%s",
                    error.empty() ? "None" : error.c_str());
        return;
    }

    m_result = true;
    SE_LOG_INFO(NULL, NULL, "Session.Abort callback successfull");
}

void SessionResource::getDatabases(const string &sourceName, ReadOperations::SourceDatabases_t &databases)
{
    resetReplies();
    m_sessionProxy->m_getDatabases(sourceName, boost::bind(&SessionResource::getDatabasesCb, this,
                                                           &databases, _1, _2));
    waitForReply();
}

void SessionResource::getDatabasesCb(ReadOperations::SourceDatabases_t *databases,
                                     const ReadOperations::SourceDatabases_t &rDatabases,
                                     const std::string &error)
{
    if(!error.empty()) {
        m_result = false;
        SE_LOG_INFO(NULL, NULL, "Session.GetDatabases callback returned: error=%s",
                    error.empty() ? "None" : error.c_str());
    } else {
        m_result = true;
        *databases    = rDatabases;
        SE_LOG_INFO(NULL, NULL, "Session.GetDatabases callback returned: success");
    }

    replyInc();
}

SessionListener* SessionResource::addListener(SessionListener *listener)
{
    return listener;
}

void SessionResource::statusChangedCb(const std::string &status, uint32_t error,
                                      const SessionCommon::SourceStatuses_t &sources)
{
    SE_LOG_INFO(NULL, NULL, "Session.StatusChanged signal received: status=%s", status.c_str());
    return;
}

void SessionResource::progressChangedCb(int32_t error, const SessionCommon::SourceProgresses_t &sources)
{
    SE_LOG_INFO(NULL, NULL, "Session.ProgressChanged signal received: error=%d", error);
    return;
}

void SessionResource::onSessionConnect(const GDBusCXX::DBusConnectionPtr &conn)
{
    SE_LOG_INFO(NULL, NULL, "SessionProxy interface end with: %s", m_sessionID.c_str());
    m_sessionProxy.reset(new SessionProxy(conn, m_sessionID));

    /* Enable public dbus interface for Session. */
    activate();
    replyInc(); // Init is waiting on a reply.

    // Activate signal watch on helper signals.
    m_sessionProxy->m_statusChanged.activate  (boost::bind(&SessionResource::statusChangedCb,   this, _1, _2, _3));
    m_sessionProxy->m_progressChanged.activate(boost::bind(&SessionResource::progressChangedCb, this, _1, _2));
    m_sessionProxy->m_done.activate           (boost::bind(&SessionResource::done,              this));

    SE_LOG_INFO(NULL, NULL, "onSessionConnect called in session-resource (path: %s interface: %s)",
                m_sessionProxy->getPath(), m_sessionProxy->getInterface());

    SE_LOG_INFO(NULL, NULL, "Session connection made.");
}

void SessionResource::onQuit(int status)
{
    SE_LOG_INFO(NULL, NULL, "dbus-helper quit with status: %d", status);
}

void SessionResource::onFailure(const std::string &error)
{
    SE_LOG_INFO(NULL, NULL, "dbus-helper failed with error: %s", error.c_str());
}

boost::shared_ptr<SessionResource> SessionResource::createSessionResource(Server &server,
                                                                          const std::string &peerDeviceID,
                                                                          const std::string &config_name,
                                                                          const std::string &session,
                                                                          const std::vector<std::string> &flags)
{
    boost::shared_ptr<SessionResource> me(new SessionResource(server, peerDeviceID,
                                                              config_name, session, flags));
    me->init();
    me->m_me = me;
    return me;
}

SessionResource::SessionResource(Server &server,
                                 const std::string &peerDeviceID,
                                 const std::string &configName,
                                 const std::string &session,
                                 const std::vector<std::string> &flags) :
    DBusObjectHelper(server.getConnection(),
                     std::string("/org/syncevolution/Session/") + session,
                     "org.syncevolution.Session",
                     boost::bind(&Server::autoTermCallback, &server)),
    m_server(server),
    m_flags(flags),
    m_sessionID(session),
    m_peerDeviceID(peerDeviceID),
    m_path(std::string("/org/syncevolution/Session/") + session),
    m_configName(configName),
    m_setConfig(false),
    m_done(false),
    m_forkExecParent(SyncEvo::ForkExecParent::create("syncevo-dbus-helper")),
    emitStatus(*this, "StatusChanged"),
    emitProgress(*this, "ProgressChanged")
{
    add(this, &SessionResource::attach, "Attach");
    add(this, &SessionResource::detach, "Detach");
    add(this, &SessionResource::getFlags, "GetFlags");
    add(this, &SessionResource::getNormalConfigName, "GetConfigName");
    add(this, &SessionResource::getConfigs, "GetConfigs");
    add(this, &SessionResource::getConfig, "GetConfig");
    add(this, &SessionResource::getNamedConfig, "GetNamedConfig");
    add(this, &SessionResource::setConfig, "SetConfig");
    add(this, &SessionResource::setNamedConfig, "SetNamedConfig");
    add(this, &SessionResource::getReports, "GetReports");
    add(this, &SessionResource::checkSource, "CheckSource");
    add(this, &SessionResource::getDatabases, "GetDatabases");
    add(this, &SessionResource::sync, "Sync");
    add(this, &SessionResource::abort, "Abort");
    add(this, &SessionResource::suspend, "Suspend");
    add(this, &SessionResource::getStatus, "GetStatus");
    add(this, &SessionResource::getProgress, "GetProgress");
    add(this, &SessionResource::restore, "Restore");
    add(this, &SessionResource::checkPresence, "checkPresence");
    add(this, &SessionResource::execute, "Execute");
    add(emitStatus);
    add(emitProgress);

    SE_LOG_DEBUG(NULL, NULL, "session resource %s created", getPath());
}

void SessionResource::waitForReply(gint timeout)
{
    m_result = true;
    gint fd = GDBusCXX::dbus_get_connection_fd(getConnection());

    // Wakeup for any activity on the connection's fd.
    GPollFD pollFd = { fd, G_IO_IN | G_IO_OUT | G_IO_HUP | G_IO_ERR, 0 };
    while(!methodInvocationDone()) {
        // Block until there is activity on the connection or 100ms elapses.
        if (!g_poll(&pollFd, 1, timeout)) {
            m_result = false; // This is taking too long. Get out of here.
        } else if (pollFd.revents & (G_IO_HUP | G_IO_ERR)) {
            m_result = false; // Error of connection lost
        }

        if(!m_result) {
            replyInc();
            return;
        } else {
            // Allow for processing of new message.
            g_main_context_iteration(g_main_context_default(), TRUE);
        }
    }
}

void SessionResource::done()
{
    if (m_done) {
        return;
    }
    SE_LOG_DEBUG(NULL, NULL, "session %s done", getPath());

    /* update auto sync manager when a config is changed */
    if (m_setConfig) {
        m_server.getAutoSyncManager().update(m_configName);
    }
    m_server.removeSession(this);

    // now tell other clients about config change?
    if (m_setConfig) {
        m_server.configChanged();
    }
}

SessionResource::~SessionResource()
{
    SE_LOG_DEBUG(NULL, NULL, "session resource %s deconstructing", getPath());
    done();
}

SE_END_CXX
