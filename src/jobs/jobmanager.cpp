/*
Copyright (C) 2014  Jean-Baptiste Mardelle <jb@kdenlive.org>
Copyright (C) 2017  Nicolas Carion
This file is part of Kdenlive. See www.kdenlive.org.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of
the License or (at your option) version 3 or any later version
accepted by the membership of KDE e.V. (or its successor approved
by the membership of KDE e.V.), which shall act as a proxy
defined in Section 14 of version 3 of the license.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "jobmanager.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "macros.hpp"
#include "undohelper.hpp"

#include <QFuture>
#include <QFutureWatcher>
#include <QThread>

int JobManager::m_currentId = 0;
JobManager::JobManager()
    : QAbstractListModel()
    , m_lock(QReadWriteLock::Recursive)
{
}

JobManager::~JobManager()
{
    slotCancelJobs();
}

std::vector<int> JobManager::getPendingJobsIds(const QString &id, AbstractClipJob::JOBTYPE type)
{
    READ_LOCK();
    std::vector<int> result;
    for (int jobId : m_jobsByClip.at(id)) {
        if (!m_jobs.at(jobId)->m_future.isFinished() && !m_jobs.at(jobId)->m_future.isCanceled()) {
            if (type == AbstractClipJob::NOJOBTYPE || m_jobs.at(jobId)->m_type == type) {
                result.push_back(jobId);
            }
        }
    }
    return result;
}

std::vector<int> JobManager::getFinishedJobsIds(const QString &id, AbstractClipJob::JOBTYPE type)
{
    READ_LOCK();
    std::vector<int> result;
    for (int jobId : m_jobsByClip.at(id)) {
        if (m_jobs.at(jobId)->m_future.isFinished() || m_jobs.at(jobId)->m_future.isCanceled()) {
            if (type == AbstractClipJob::NOJOBTYPE || m_jobs.at(jobId)->m_type == type) {
                result.push_back(jobId);
            }
        }
    }
    return result;
}

void JobManager::discardJobs(const QString &binId, AbstractClipJob::JOBTYPE type)
{
    QWriteLocker locker(&m_lock);
    if (m_jobsByClip.count(binId) == 0) {
        return;
    }
    for (int jobId : m_jobsByClip.at(binId)) {
        if (type == AbstractClipJob::NOJOBTYPE || m_jobs.at(jobId)->m_type == type) {
            m_jobs.at(jobId)->m_future.cancel();
        }
    }
}

bool JobManager::hasPendingJob(const QString &clipId, AbstractClipJob::JOBTYPE type, int *foundId)
{
    READ_LOCK();
    for (int jobId : m_jobsByClip.at(clipId)) {
        if ((type == AbstractClipJob::NOJOBTYPE || m_jobs.at(jobId)->m_type == type) && !m_jobs.at(jobId)->m_future.isFinished() &&
            !m_jobs.at(jobId)->m_future.isCanceled()) {
            if (foundId) {
                *foundId = jobId;
            }
            return true;
        }
    }
    if (foundId) {
        *foundId = -1;
    }
    return false;
}

void JobManager::updateJobCount()
{
    READ_LOCK();
    int count = 0;
    for (const auto &j : m_jobs) {
        if (!j.second->m_future.isFinished() && !j.second->m_future.isCanceled()) {
            for (int i = 0; i < j.second->m_future.future().resultCount(); ++i) {
                if (j.second->m_future.future().isResultReadyAt(i)) {
                    count++;
                }
            }
        }
    }
    // Set jobs count
    emit jobCount(count);
}

/*
void JobManager::prepareJobs(const QList<ProjectClip *> &clips, double fps, AbstractClipJob::JOBTYPE jobType, const QStringList &params)
{
    // TODO filter clips
    QList<ProjectClip *> matching = filterClips(clips, jobType, params);
    if (matching.isEmpty()) {
        m_bin->doDisplayMessage(i18n("No valid clip to process"), KMessageWidget::Information);
        return;
    }
    QHash<ProjectClip *, AbstractClipJob *> jobs;
    if (jobType == AbstractClipJob::TRANSCODEJOB) {
        jobs = CutClipJob::prepareTranscodeJob(fps, matching, params);
    } else if (jobType == AbstractClipJob::CUTJOB) {
        ProjectClip *clip = matching.constFirst();
        double originalFps = clip->getOriginalFps();
        jobs = CutClipJob::prepareCutClipJob(fps, originalFps, clip);
    } else if (jobType == AbstractClipJob::ANALYSECLIPJOB) {
        jobs = CutClipJob::prepareAnalyseJob(fps, matching, params);
    } else if (jobType == AbstractClipJob::FILTERCLIPJOB) {
        jobs = FilterJob::prepareJob(matching, params);
    } else if (jobType == AbstractClipJob::PROXYJOB) {
        jobs = ProxyJob::prepareJob(m_bin, matching);
    }
    if (!jobs.isEmpty()) {
        QHashIterator<ProjectClip *, AbstractClipJob *> i(jobs);
        while (i.hasNext()) {
            i.next();
            launchJob(i.key(), i.value(), false);
        }
        slotCheckJobProcess();
    }
}
*/

void JobManager::slotDiscardClipJobs(const QString &binId)
{
    QWriteLocker locker(&m_lock);
    for (int jobId : m_jobsByClip.at(binId)) {
        Q_ASSERT(m_jobs.count(jobId) > 0);
        m_jobs[jobId]->m_future.cancel();
    }
}

void JobManager::slotCancelPendingJobs()
{
    QWriteLocker locker(&m_lock);
    for (const auto &j : m_jobs) {
        if (!j.second->m_future.isStarted()) {
            j.second->m_future.cancel();
        }
    }
}

void JobManager::slotCancelJobs()
{
    QWriteLocker locker(&m_lock);
    for (const auto &j : m_jobs) {
        j.second->m_future.cancel();
    }
}

void JobManager::createJob(std::shared_ptr<Job_t> job, const std::vector<int> &parents)
{
    qDebug() << "################### Createq JOB"<<job->m_id;
    bool ok = false;
    // wait for parents to finish
    while (!ok) {
        ok = true;
        for (int p : parents) {
            if (!m_jobs[p]->m_completionMutex.try_lock()) {
                ok = false;
                break;
            } else {
                m_jobs[p]->m_completionMutex.unlock();
            }
        }
        if (!ok) {
            QThread::sleep(1);
        }
    }
    qDebug() << "################### Create JOB STARTING"<<job->m_id;
    // connect progress signals
    for (const auto &it : job->m_indices) {
        size_t i = it.second;
        auto binId = it.first;
        connect(job->m_job[i].get(), &AbstractClipJob::jobProgress, [job, i, binId](int p) {
            job->m_progress[i] = std::max(job->m_progress[i], p);
            pCore->projectItemModel()->onItemUpdated(binId);
        });
    }
    QWriteLocker locker(&m_lock);
    job->m_actualFuture = QtConcurrent::mapped(job->m_job, AbstractClipJob::execute);
    job->m_future.setFuture(job->m_actualFuture);
    connect(&job->m_future, &QFutureWatcher<bool>::finished, [ this, id = job->m_id ]() { slotManageFinishedJob(id); });
    connect(&job->m_future, &QFutureWatcher<bool>::canceled, [ this, id = job->m_id ]() { slotManageCanceledJob(id); });
    connect(&job->m_future, &QFutureWatcher<bool>::started, this, &JobManager::updateJobCount);
    connect(&job->m_future, &QFutureWatcher<bool>::finished, this, &JobManager::updateJobCount);
    connect(&job->m_future, &QFutureWatcher<bool>::canceled, this, &JobManager::updateJobCount);

    // In the unlikely event that the job finished before the signal connection was made, we check manually for finish and cancel
    if (job->m_future.isFinished()) {
        emit job->m_future.finished();
    }
    if (job->m_future.isCanceled()) {
        emit job->m_future.canceled();
    }
}

void JobManager::slotManageCanceledJob(int id)
{
    QWriteLocker locker(&m_lock);
    Q_ASSERT(m_jobs.count(id) > 0);
    if (m_jobs[id]->m_processed) return;
    m_jobs[id]->m_processed = true;
    m_jobs[id]->m_completionMutex.unlock();
    // send notification to refresh view
    for (const auto &it : m_jobs[id]->m_indices) {
        pCore->projectItemModel()->onItemUpdated(it.first);
    }
    updateJobCount();
}
void JobManager::slotManageFinishedJob(int id)
{
    qDebug() << "################### JOB finished"<<id;
    QWriteLocker locker(&m_lock);
    Q_ASSERT(m_jobs.count(id) > 0);
    if (m_jobs[id]->m_processed) return;
    m_jobs[id]->m_processed = true;
    // send notification to refresh view
    for (const auto &it : m_jobs[id]->m_indices) {
        pCore->projectItemModel()->onItemUpdated(it.first);
    }
    bool ok = true;
    for (bool res : m_jobs[id]->m_future.future()) {
        ok = ok && res;
    }
    if (!ok) return;
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    for (const auto &j : m_jobs[id]->m_job) {
        ok = ok && j->commitResult(undo, redo);
    }
    if (!ok) {
        qDebug() << "ERROR: Job " << id << " failed";
        m_jobs[id]->m_failed = true;
    }
    if (ok && !m_jobs[id]->m_undoString.isEmpty()) {
        pCore->pushUndo(undo, redo, m_jobs[id]->m_undoString);
    }
    m_jobs[id]->m_completionMutex.unlock();
    updateJobCount();
}

AbstractClipJob::JOBTYPE JobManager::getJobType(int jobId) const
{
    READ_LOCK();
    Q_ASSERT(m_jobs.count(jobId) > 0);
    return m_jobs.at(jobId)->m_type;
}

JobManagerStatus JobManager::getJobStatus(int jobId) const
{
    READ_LOCK();
    Q_ASSERT(m_jobs.count(jobId) > 0);
    auto job = m_jobs.at(jobId);
    if (job->m_future.isFinished()) {
        return JobManagerStatus::Finished;
    }
    if (job->m_future.isCanceled()) {
        return JobManagerStatus::Canceled;
    }
    if (job->m_future.isRunning()) {
        return JobManagerStatus::Running;
    }
    return JobManagerStatus::Pending;
}

int JobManager::getJobProgressForClip(int jobId, const QString &binId) const
{
    READ_LOCK();
    Q_ASSERT(m_jobs.count(jobId) > 0);
    auto job = m_jobs.at(jobId);
    Q_ASSERT(job->m_indices.count(binId) > 0);
    size_t ind = job->m_indices.at(binId);
    return job->m_progress[ind];
}

QString JobManager::getJobMessageForClip(int jobId, const QString &binId) const
{
    READ_LOCK();
    Q_ASSERT(m_jobs.count(jobId) > 0);
    auto job = m_jobs.at(jobId);
    Q_ASSERT(job->m_indices.count(binId) > 0);
    size_t ind = job->m_indices.at(binId);
    return job->m_job[ind]->getErrorMessage();
}

QVariant JobManager::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) {
        return QVariant();
    }
    int row = index.row();
    if (row >= int(m_jobs.size()) || row < 0) {
        return QVariant();
    }
    auto it = m_jobs.begin();
    std::advance(it, row);
    switch (role) {
    case Qt::DisplayRole:
        return QVariant(it->second->m_job.front()->getDescription());
        break;
    }
    return QVariant();
}

int JobManager::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return int(m_jobs.size());
}