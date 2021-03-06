/**************************************************************************
* Otter Browser: Web browser controlled by the user, not vice-versa.
* Copyright (C) 2013 - 2014 Michal Dutkiewicz aka Emdek <michal@emdek.pl>
* Copyright (C) 2015 Jan Bajer aka bajasoft <jbajer@gmail.com>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
**************************************************************************/

#include "TransfersManager.h"
#include "NetworkManager.h"
#include "SessionsManager.h"
#include "SettingsManager.h"
#include "WebBackend.h"
#include "WebBackendsManager.h"
#include "../ui/MainWindow.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QMimeDatabase>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryFile>
#include <QtCore/QTimer>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMessageBox>

namespace Otter
{

TransfersManager* TransfersManager::m_instance = NULL;
NetworkManager* TransfersManager::m_networkManager = NULL;
QHash<QNetworkReply*, TransferInformation*> TransfersManager::m_replies;
QList<TransferInformation*> TransfersManager::m_transfers;

TransfersManager::TransfersManager(QObject *parent) : QObject(parent),
	m_updateTimer(0)
{
	QSettings history(SessionsManager::getProfilePath() + QLatin1String("/transfers.ini"), QSettings::IniFormat);
	const QStringList entries = history.childGroups();

	for (int i = 0; i < entries.count(); ++i)
	{
		TransferInformation *transfer = new TransferInformation();
		transfer->source = history.value(QStringLiteral("%1/source").arg(entries.at(i))).toString();
		transfer->target = history.value(QStringLiteral("%1/target").arg(entries.at(i))).toString();
		transfer->started = history.value(QStringLiteral("%1/started").arg(entries.at(i))).toDateTime();
		transfer->finished = history.value(QStringLiteral("%1/finished").arg(entries.at(i))).toDateTime();
		transfer->mimeType = QMimeDatabase().mimeTypeForFile(transfer->target);
		transfer->bytesTotal = history.value(QStringLiteral("%1/bytesTotal").arg(entries.at(i))).toLongLong();
		transfer->bytesReceived = history.value(QStringLiteral("%1/bytesReceived").arg(entries.at(i))).toLongLong();
		transfer->state = ((transfer->bytesReceived > 0 && transfer->bytesTotal == transfer->bytesReceived) ? FinishedTransfer : ErrorTransfer);

		m_transfers.append(transfer);
	}

	connect(QCoreApplication::instance(), SIGNAL(aboutToQuit()), this, SLOT(save()));
}

TransfersManager::~TransfersManager()
{
	for (int i = (m_transfers.count() - 1); i >= 0; --i)
	{
		delete m_transfers.takeAt(i);
	}
}

void TransfersManager::createInstance(QObject *parent)
{
	if (!m_instance)
	{
		m_instance = new TransfersManager(parent);
	}
}

void TransfersManager::timerEvent(QTimerEvent *event)
{
	Q_UNUSED(event)

	QHash<QNetworkReply*, TransferInformation*>::iterator iterator;

	for (iterator = m_replies.begin(); iterator != m_replies.end(); ++iterator)
	{
		iterator.value()->speed = (iterator.value()->bytesReceivedDifference * 2);
		iterator.value()->bytesReceivedDifference = 0;

		if (!iterator.value()->isHidden)
		{
			emit m_instance->transferUpdated(iterator.value());
		}
	}

	save();

	if (m_replies.isEmpty())
	{
		killTimer(m_updateTimer);

		m_updateTimer = 0;
	}
}

void TransfersManager::startUpdates()
{
	if (m_updateTimer == 0)
	{
		m_updateTimer = startTimer(500);
	}
}

void TransfersManager::downloadProgress(qint64 bytesReceived, qint64 bytesTotal)
{
	QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());

	if (!reply || !m_replies.contains(reply))
	{
		return;
	}

	m_replies[reply]->bytesReceivedDifference += (bytesReceived - (m_replies[reply]->bytesReceived - m_replies[reply]->bytesStart));
	m_replies[reply]->bytesReceived = (m_replies[reply]->bytesStart + bytesReceived);
	m_replies[reply]->bytesTotal = (m_replies[reply]->bytesStart + bytesTotal);
}

void TransfersManager::downloadData(QNetworkReply *reply)
{
	if (!reply)
	{
		reply = qobject_cast<QNetworkReply*>(sender());
	}

	if (!reply || !m_replies.contains(reply))
	{
		return;
	}

	TransferInformation *transfer = m_replies[reply];

	if (transfer->state == ErrorTransfer)
	{
		transfer->state = RunningTransfer;

		if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid() && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 206)
		{
			transfer->device->reset();
		}
	}

	transfer->device->write(reply->readAll());
}

void TransfersManager::downloadFinished(QNetworkReply *reply)
{
	if (!reply)
	{
		reply = qobject_cast<QNetworkReply*>(sender());
	}

	if (!reply || !m_replies.contains(reply))
	{
		return;
	}

	TransferInformation *transfer = m_replies[reply];

	if (reply->size() > 0)
	{
		transfer->device->write(reply->readAll());
	}

	disconnect(reply, SIGNAL(downloadProgress(qint64,qint64)), m_instance, SLOT(downloadProgress(qint64,qint64)));
	disconnect(reply, SIGNAL(readyRead()), m_instance, SLOT(downloadData()));
	disconnect(reply, SIGNAL(finished()), m_instance, SLOT(downloadFinished()));

	transfer->state = FinishedTransfer;
	transfer->finished = QDateTime::currentDateTime();
	transfer->bytesReceived = (transfer->device ? transfer->device->size() : -1);

	if (transfer->bytesTotal <= 0 && transfer->bytesReceived > 0)
	{
		transfer->bytesTotal = transfer->bytesReceived;
	}

	if (transfer->bytesReceived == 0 || transfer->bytesReceived < transfer->bytesTotal)
	{
		transfer->state = ErrorTransfer;
	}
	else
	{
		transfer->mimeType = QMimeDatabase().mimeTypeForFile(transfer->target);
	}

	if (!transfer->isHidden)
	{
		emit m_instance->transferFinished(transfer);
		emit m_instance->transferUpdated(transfer);
	}

	if (transfer->device && !transfer->device->inherits(QStringLiteral("QTemporaryFile").toLatin1()))
	{
		transfer->device->close();
		transfer->device->deleteLater();
		transfer->device = NULL;

		m_replies.remove(reply);

		QTimer::singleShot(250, reply, SLOT(deleteLater()));
	}
}

void TransfersManager::downloadError(QNetworkReply::NetworkError error)
{
	Q_UNUSED(error)

	QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());

	if (!reply || !m_replies.contains(reply))
	{
		return;
	}

	TransferInformation *transfer = m_replies[reply];

	stopTransfer(transfer);

	transfer->state = ErrorTransfer;
}

void TransfersManager::save()
{
	QSettings history(SessionsManager::getProfilePath() + QLatin1String("/transfers.ini"), QSettings::IniFormat);
	history.clear();

	if (SettingsManager::getValue(QLatin1String("Browser/PrivateMode")).toBool() || !SettingsManager::getValue(QLatin1String("History/RememberDownloads")).toBool())
	{
		return;
	}

	const int limit = SettingsManager::getValue(QLatin1String("History/DownloadsLimitPeriod")).toInt();
	int entry = 1;

	for (int i = 0; i < m_transfers.count(); ++i)
	{
		if (m_transfers.at(i)->isPrivate || m_transfers.at(i)->isHidden || (m_transfers.at(i)->state == FinishedTransfer && m_transfers.at(i)->finished.isValid() && m_transfers.at(i)->finished.daysTo(QDateTime::currentDateTime()) > limit))
		{
			continue;
		}

		history.setValue(QStringLiteral("%1/source").arg(entry), m_transfers.at(i)->source);
		history.setValue(QStringLiteral("%1/target").arg(entry), m_transfers.at(i)->target);
		history.setValue(QStringLiteral("%1/started").arg(entry), m_transfers.at(i)->started);
		history.setValue(QStringLiteral("%1/finished").arg(entry), ((m_transfers.at(i)->finished.isValid() && m_transfers.at(i)->state != RunningTransfer) ? m_transfers.at(i)->finished : QDateTime::currentDateTime()));
		history.setValue(QStringLiteral("%1/bytesTotal").arg(entry), m_transfers.at(i)->bytesTotal);
		history.setValue(QStringLiteral("%1/bytesReceived").arg(entry), m_transfers.at(i)->bytesReceived);

		++entry;
	}

	history.sync();
}

void TransfersManager::clearTransfers(int period)
{
	const QList<TransferInformation*> transfers = m_transfers;

	for (int i = 0; i < transfers.count(); ++i)
	{
		if (transfers.at(i)->state == FinishedTransfer && (period == 0 || (transfers.at(i)->finished.isValid() && transfers.at(i)->finished.secsTo(QDateTime::currentDateTime()) > (period * 3600))))
		{
			TransfersManager::removeTransfer(transfers.at(i));
		}
	}
}

TransfersManager* TransfersManager::getInstance()
{
	return m_instance;
}

TransferInformation* TransfersManager::startTransfer(const QString &source, const QString &target, bool privateTransfer, bool quickTransfer, bool skipTransfers)
{
	QNetworkRequest request;
	request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
	request.setHeader(QNetworkRequest::UserAgentHeader, WebBackendsManager::getBackend()->getUserAgent());
	request.setUrl(QUrl(source));

	return startTransfer(request, target, privateTransfer, quickTransfer, skipTransfers);
}

TransferInformation* TransfersManager::startTransfer(const QNetworkRequest &request, const QString &target, bool privateTransfer, bool quickTransfer, bool skipTransfers)
{
	if (!m_networkManager)
	{
		m_networkManager = new NetworkManager(true, m_instance);
		m_networkManager->setParent(m_instance);
	}

	return startTransfer(m_networkManager->get(request), target, privateTransfer, quickTransfer, skipTransfers);
}

TransferInformation* TransfersManager::startTransfer(QNetworkReply *reply, const QString &target, bool privateTransfer, bool quickTransfer, bool skipTransfers)
{
	if (!reply)
	{
		return NULL;
	}

	QPointer<QNetworkReply> replyPointer = reply;
	QTemporaryFile temporaryFile(QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QDir::separator() + QLatin1String("otter-download-XXXXXX.dat"), m_instance);
	TransferInformation *transfer = new TransferInformation();
	transfer->source = reply->url().toString(QUrl::RemovePassword | QUrl::PreferLocalFile);
	transfer->device = &temporaryFile;
	transfer->started = QDateTime::currentDateTime();
	transfer->mimeType = QMimeDatabase().mimeTypeForName(reply->header(QNetworkRequest::ContentTypeHeader).toString());
	transfer->bytesTotal = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
	transfer->isPrivate = privateTransfer;
	transfer->isHidden = skipTransfers;

	if (!transfer->device->open(QIODevice::ReadWrite))
	{
		delete transfer;

		return NULL;
	}

	transfer->state = (reply->isFinished() ? FinishedTransfer : RunningTransfer);

	m_instance->downloadData(reply);

	m_transfers.append(transfer);

	const bool isRunning = (transfer->state == RunningTransfer);

	if (isRunning)
	{
		m_replies[reply] = transfer;

		connect(reply, SIGNAL(downloadProgress(qint64,qint64)), m_instance, SLOT(downloadProgress(qint64,qint64)));
		connect(reply, SIGNAL(finished()), m_instance, SLOT(downloadFinished()));
		connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), m_instance, SLOT(downloadError(QNetworkReply::NetworkError)));
	}
	else
	{
		transfer->finished = QDateTime::currentDateTime();
	}

	transfer->device->reset();

	transfer->mimeType = QMimeDatabase().mimeTypeForData(transfer->device);

	transfer->device->seek(transfer->device->size());

	m_instance->downloadData(reply);

	if (isRunning)
	{
		connect(reply, SIGNAL(readyRead()), m_instance, SLOT(downloadData()));
	}

	if (target.isEmpty())
	{
		QUrl url;
		QString fileName;

		if (reply->hasRawHeader(QStringLiteral("Content-Disposition").toLatin1()))
		{
			url = QUrl(QRegularExpression(QLatin1String(" filename=\"?([^\"]+)\"?")).match(QString(reply->rawHeader(QStringLiteral("Content-Disposition").toLatin1()))).captured(1));

			fileName = url.fileName();
		}

		if (fileName.isEmpty())
		{
			url = QUrl(transfer->source);

			fileName = url.fileName();
		}

		if (fileName.isEmpty())
		{
			fileName = tr("file");
		}

		if (QFileInfo(fileName).suffix().isEmpty())
		{
			QString suffix;

			if (reply->header(QNetworkRequest::ContentTypeHeader).isValid())
			{
				suffix = transfer->mimeType.preferredSuffix();
			}

			if (!suffix.isEmpty())
			{
				fileName.append('.');
				fileName.append(suffix);
			}
		}

		QString path;

		if (!quickTransfer && !SettingsManager::getValue(QLatin1String("Browser/AlwaysAskWhereToSaveDownload")).toBool())
		{
			quickTransfer = true;
		}

		if (quickTransfer)
		{
			path = SettingsManager::getValue(QLatin1String("Paths/Downloads")).toString() + QDir::separator() + fileName;

			if (QFile::exists(path) && QMessageBox::question(SessionsManager::getActiveWindow(), tr("Question"), tr("File with that name already exists.\nDo you want to overwite it?"), (QMessageBox::Yes | QMessageBox::No)) == QMessageBox::No)
			{
				path = QString();
			}
		}

		path = getSavePath(fileName, path);

		if (path.isEmpty())
		{
			transfer->device = NULL;

			m_replies.remove(reply);

			removeTransfer(transfer, false);

			if (replyPointer)
			{
				reply->abort();
			}

			return NULL;
		}

		transfer->target = QDir::toNativeSeparators(path);
	}
	else
	{
		transfer->target = QFileInfo(QDir::toNativeSeparators(target)).canonicalFilePath();
	}

	if (!target.isEmpty() && QFile::exists(transfer->target) && QMessageBox::question(SessionsManager::getActiveWindow(), tr("Question"), tr("File with the same name already exists.\nDo you want to overwrite it?\n\n%1").arg(transfer->target), (QMessageBox::Yes | QMessageBox::Cancel)) == QMessageBox::Cancel)
	{
		removeTransfer(transfer, false);

		return NULL;
	}

	QFile *file = new QFile(transfer->target);

	if (!file->open(QIODevice::WriteOnly))
	{
		removeTransfer(transfer, false);

		return NULL;
	}

	if (m_replies.contains(reply))
	{
		if (transfer->state == RunningTransfer && replyPointer)
		{
			disconnect(reply, SIGNAL(readyRead()), m_instance, SLOT(downloadData()));
		}
		else
		{
			m_replies.remove(reply);
		}
	}

	temporaryFile.reset();

	file->write(temporaryFile.readAll());

	transfer->device = file;

	if (m_replies.contains(reply) && replyPointer)
	{
		if (reply->isFinished())
		{
			m_instance->downloadFinished(reply);

			transfer->device = NULL;
		}
		else
		{
			m_instance->downloadData(reply);
		}

		connect(reply, SIGNAL(readyRead()), m_instance, SLOT(downloadData()));
	}
	else
	{
		transfer->device = NULL;
	}

	temporaryFile.close();

	if (transfer->state == FinishedTransfer)
	{
		if (transfer->bytesTotal <= 0 && transfer->bytesReceived > 0)
		{
			transfer->bytesTotal = transfer->bytesReceived;
		}

		if (transfer->bytesReceived == 0 || transfer->bytesReceived < transfer->bytesTotal)
		{
			transfer->state = ErrorTransfer;
		}
		else
		{
			transfer->mimeType = QMimeDatabase().mimeTypeForFile(transfer->target);
		}
	}

	if (!transfer->isHidden)
	{
		emit m_instance->transferStarted(transfer);
	}

	if (m_replies.contains(reply) && replyPointer)
	{
		m_instance->startUpdates();
	}
	else
	{
		file->close();
		file->deleteLater();

		if (!transfer->isHidden)
		{
			emit m_instance->transferFinished(transfer);
		}
	}

	return transfer;
}

QString TransfersManager::getSavePath(const QString &fileName, QString path)
{
	do
	{
		if (path.isEmpty())
		{
			QFileDialog dialog(SessionsManager::getActiveWindow(), tr("Save File"), SettingsManager::getValue(QLatin1String("Paths/SaveFile")).toString() + QDir::separator() + fileName, tr("All files (*)"));
			dialog.setFileMode(QFileDialog::AnyFile);
			dialog.setAcceptMode(QFileDialog::AcceptSave);

			if (dialog.exec() == QDialog::Rejected || dialog.selectedFiles().isEmpty())
			{
				break;
			}

			path = dialog.selectedFiles().first();
		}

		const bool exists = QFile::exists(path);

		if (isDownloading(QString(), path))
		{
			path = QString();

			if (QMessageBox::warning(SessionsManager::getActiveWindow(), tr("Warning"), tr("Target path is already used by another transfer.\nSelect another one."), (QMessageBox::Ok | QMessageBox::Cancel)) == QMessageBox::Cancel)
			{
				break;
			}
		}
		else if ((exists && !QFileInfo(path).isWritable()) || (!exists && !QFileInfo(QFileInfo(path).dir().path()).isWritable()))
		{
			path = QString();

			if (QMessageBox::warning(SessionsManager::getActiveWindow(), tr("Warning"), tr("Target path is not writable.\nSelect another one."), (QMessageBox::Ok | QMessageBox::Cancel)) == QMessageBox::Cancel)
			{
				break;
			}
		}
		else
		{
			break;
		}
	}
	while (true);

	if (!path.isEmpty())
	{
		SettingsManager::setValue(QLatin1String("Paths/SaveFile"), QFileInfo(path).dir().canonicalPath());
	}

	return path;
}

QList<TransferInformation*> TransfersManager::getTransfers()
{
	return m_transfers;
}

bool TransfersManager::resumeTransfer(TransferInformation *transfer)
{
	if (!m_transfers.contains(transfer) || m_replies.key(transfer) || transfer->state != ErrorTransfer || !QFile::exists(transfer->target))
	{
		return false;
	}

	if (transfer->bytesTotal == 0)
	{
		return restartTransfer(transfer);
	}

	QFile *file = new QFile(transfer->target);

	if (!file->open(QIODevice::WriteOnly | QIODevice::Append))
	{
		return false;
	}

	transfer->device = file;
	transfer->started = QDateTime::currentDateTime();
	transfer->bytesStart = file->size();

	QNetworkRequest request;
	request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
	request.setHeader(QNetworkRequest::UserAgentHeader, WebBackendsManager::getBackend()->getUserAgent());
	request.setRawHeader(QStringLiteral("Range").toLatin1(), QStringLiteral("bytes=%1-").arg(file->size()).toLatin1());
	request.setUrl(QUrl(transfer->source));

	if (!m_networkManager)
	{
		m_networkManager = new NetworkManager(true, m_instance);
		m_networkManager->setParent(m_instance);
	}

	QNetworkReply *reply = m_networkManager->get(request);

	m_replies[reply] = transfer;

	m_instance->downloadData(reply);

	connect(reply, SIGNAL(downloadProgress(qint64,qint64)), m_instance, SLOT(downloadProgress(qint64,qint64)));
	connect(reply, SIGNAL(readyRead()), m_instance, SLOT(downloadData()));
	connect(reply, SIGNAL(finished()), m_instance, SLOT(downloadFinished()));
	connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), m_instance, SLOT(downloadError(QNetworkReply::NetworkError)));

	m_instance->startUpdates();

	return true;
}

bool TransfersManager::restartTransfer(TransferInformation *transfer)
{
	if (!transfer || !m_transfers.contains(transfer))
	{
		return false;
	}

	stopTransfer(transfer);

	QFile *file = new QFile(transfer->target);

	if (!file->open(QIODevice::WriteOnly))
	{
		return false;
	}

	transfer->device = file;
	transfer->started = QDateTime::currentDateTime();
	transfer->bytesStart = 0;

	QNetworkRequest request;
	request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
	request.setHeader(QNetworkRequest::UserAgentHeader, WebBackendsManager::getBackend()->getUserAgent());
	request.setUrl(QUrl(transfer->source));

	if (!m_networkManager)
	{
		m_networkManager = new NetworkManager(true, m_instance);
		m_networkManager->setParent(m_instance);
	}

	QNetworkReply *reply = m_networkManager->get(request);

	m_replies[reply] = transfer;

	m_instance->downloadData(reply);

	connect(reply, SIGNAL(downloadProgress(qint64,qint64)), m_instance, SLOT(downloadProgress(qint64,qint64)));
	connect(reply, SIGNAL(readyRead()), m_instance, SLOT(downloadData()));
	connect(reply, SIGNAL(finished()), m_instance, SLOT(downloadFinished()));
	connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), m_instance, SLOT(downloadError(QNetworkReply::NetworkError)));

	m_instance->startUpdates();

	return true;
}

bool TransfersManager::removeTransfer(TransferInformation *transfer, bool keepFile)
{
	if (!transfer || !m_transfers.contains(transfer))
	{
		return false;
	}

	stopTransfer(transfer);

	if (!keepFile && !transfer->target.isEmpty() && QFile::exists(transfer->target))
	{
		QFile::remove(transfer->target);
	}

	m_transfers.removeAll(transfer);

	if (!transfer->isHidden)
	{
		emit m_instance->transferRemoved(transfer);
	}

	delete transfer;

	return true;
}

bool TransfersManager::stopTransfer(TransferInformation *transfer)
{
	QNetworkReply *reply = m_replies.key(transfer);

	if (reply)
	{
		reply->abort();

		QTimer::singleShot(250, reply, SLOT(deleteLater()));

		m_replies.remove(reply);
	}

	if (transfer->device)
	{
		transfer->device->close();
		transfer->device->deleteLater();
		transfer->device = NULL;
	}

	transfer->state = ErrorTransfer;
	transfer->finished = QDateTime::currentDateTime();

	if (!transfer->isHidden)
	{
		emit m_instance->transferStopped(transfer);
		emit m_instance->transferUpdated(transfer);
	}

	return true;
}

bool TransfersManager::isDownloading(const QString &source, const QString &target)
{
	if (source.isEmpty() && target.isEmpty())
	{
		return false;
	}

	for (int i = 0; i < m_transfers.count(); ++i)
	{
		if (m_transfers.at(i)->state != RunningTransfer)
		{
			continue;
		}

		if (source.isEmpty() && m_transfers.at(i)->target == target)
		{
			return true;
		}

		if (target.isEmpty() && m_transfers.at(i)->source == source)
		{
			return true;
		}

		if (!source.isEmpty() && !target.isEmpty() && m_transfers.at(i)->source == source && m_transfers.at(i)->target == target)
		{
			return true;
		}
	}

	return false;
}

}
