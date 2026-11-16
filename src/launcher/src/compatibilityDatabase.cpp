#include "compatibilityDatabase.h"

#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QtConcurrentRun>

#include <utility>

namespace {

constexpr char FILE_NAME[]    = "compatibility_db.json";
constexpr char OVERRIDES_FILE_NAME[] = "compatibility_overrides.json";
constexpr char URL[]          = "https://kytyps5.github.io/data/compatibility.json";
constexpr int  RETRY_COUNT    = 3;
constexpr int  RETRY_DELAY_MS = 750;

struct LoadResult {
	QMap<QString, CompatibilityEntry> entries;
	QSet<QString>                    status_fields;
	QSet<QString>                    comment_fields;
	QString                           error;
};

QString TitleKey(const QString& title_id) {
	return title_id.trimmed().toUpper();
}

Configuration::GameStatus StatusFromText(const QString& text) {
	const auto value = text.trimmed();
	if (value == QStringLiteral("InGame") || value == QStringLiteral("In game")) {
		return Configuration::GameStatus::InGame;
	}
	if (value == QStringLiteral("MainMenu") || value == QStringLiteral("Main menu")) {
		return Configuration::GameStatus::MainMenu;
	}
	if (value == QStringLiteral("Logo")) {
		return Configuration::GameStatus::Logo;
	}
	if (value == QStringLiteral("DoesntBoot") || value == QStringLiteral("Doesn't boot")) {
		return Configuration::GameStatus::DoesntBoot;
	}
	return Configuration::GameStatus::Unknown;
}

QString StatusToText(Configuration::GameStatus status) {
	switch (status) {
		case Configuration::GameStatus::InGame: return QStringLiteral("InGame");
		case Configuration::GameStatus::MainMenu: return QStringLiteral("MainMenu");
		case Configuration::GameStatus::Logo: return QStringLiteral("Logo");
		case Configuration::GameStatus::DoesntBoot: return QStringLiteral("DoesntBoot");
		case Configuration::GameStatus::Unknown: return QStringLiteral("Unknown");
	}
	return QStringLiteral("Unknown");
}

LoadResult Parse(const QByteArray& data) {
	LoadResult      ret;
	QJsonParseError parse_error;
	const auto      doc = QJsonDocument::fromJson(data, &parse_error);
	if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
		ret.error = QStringLiteral("Invalid compatibility JSON: %1").arg(parse_error.errorString());
		return ret;
	}

	const auto root = doc.object();
	for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
		const auto title_id = TitleKey(it.key());
		const auto object   = it.value().toObject();
		if (title_id.isEmpty() || object.isEmpty()) {
			continue;
		}
		ret.entries.insert(title_id,
		                   {StatusFromText(object.value(QStringLiteral("status")).toString()),
		                    object.value(QStringLiteral("comment")).toString()});
		if (object.contains(QStringLiteral("status"))) {
			ret.status_fields.insert(title_id);
		}
		if (object.contains(QStringLiteral("comment"))) {
			ret.comment_fields.insert(title_id);
		}
	}
	return ret;
}

LoadResult DownloadOnce() {
	QNetworkAccessManager manager;
	QNetworkRequest       request(QUrl(QString::fromLatin1(URL)));
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
	                     QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setRawHeader("User-Agent", "Kyty-Launcher");

	auto*      reply = manager.get(request);
	QEventLoop loop;
	QTimer     timeout;
	timeout.setSingleShot(true);
	QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
	QObject::connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
	timeout.start(15000);
	loop.exec();

	if (!timeout.isActive()) {
		return {{}, {}, {}, QStringLiteral("Compatibility download timed out")};
	}
	if (reply->error() != QNetworkReply::NoError) {
		return {{}, {}, {}, reply->errorString()};
	}
	return Parse(reply->readAll());
}

LoadResult Download() {
	auto result = DownloadOnce();
	for (int retry = 1; retry <= RETRY_COUNT && !result.error.isEmpty(); retry++) {
		QThread::msleep(static_cast<unsigned long>(RETRY_DELAY_MS * retry));
		result = DownloadOnce();
	}
	return result;
}

} // namespace

CompatibilityDatabase::CompatibilityDatabase(bool local, QObject* parent)
    : QObject(parent), m_local(local) {}

const CompatibilityEntry* CompatibilityDatabase::Find(const QString& title_id) const {
	const auto entry = m_entries.constFind(TitleKey(title_id));
	return entry != m_entries.constEnd() ? &entry.value() : nullptr;
}

void CompatibilityDatabase::Load() {
	if (m_local) {
		QFile file(QDir(".").absoluteFilePath(FILE_NAME));
		if (!file.exists()) {
			emit Updated();
			return;
		}
		if (!file.open(QIODevice::ReadOnly)) {
			qWarning() << "Could not open compatibility database:" << file.errorString();
			return;
		}
		auto result = Parse(file.readAll());
		if (!result.error.isEmpty()) {
			qWarning() << result.error;
			return;
		}
		m_entries = std::move(result.entries);
		emit Updated();
		return;
	}

	QFile overrides_file(QDir(".").absoluteFilePath(OVERRIDES_FILE_NAME));
	if (overrides_file.exists()) {
		if (!overrides_file.open(QIODevice::ReadOnly)) {
			qWarning() << "Could not open compatibility overrides:" << overrides_file.errorString();
		} else {
			auto result = Parse(overrides_file.readAll());
			if (!result.error.isEmpty()) {
				qWarning() << result.error;
			} else {
				m_overrides = std::move(result.entries);
				m_status_overrides = std::move(result.status_fields);
				m_comment_overrides = std::move(result.comment_fields);
				ApplyOverrides();
				emit Updated();
			}
		}
	}

	auto* watcher = new QFutureWatcher<LoadResult>(this);
	connect(watcher, &QFutureWatcher<LoadResult>::finished, this, [this, watcher]() {
		const auto result = watcher->result();
		watcher->deleteLater();
		if (!result.error.isEmpty()) {
			qWarning() << "Could not refresh compatibility database:" << result.error;
			return;
		}
		m_entries = result.entries;
		ApplyOverrides();
		emit Updated();
	});
	watcher->setFuture(QtConcurrent::run(Download));
}

void CompatibilityDatabase::SetStatus(const QString& title_id, Configuration::GameStatus status) {
	const auto key = TitleKey(title_id);
	if (key.isEmpty()) {
		return;
	}
	m_entries[key].status = status;
	m_overrides[key]      = m_entries.value(key);
	m_status_overrides.insert(key);
	Save();
}

void CompatibilityDatabase::SetComment(const QString& title_id, const QString& comment) {
	const auto key = TitleKey(title_id);
	if (key.isEmpty()) {
		return;
	}
	m_entries[key].comment = comment;
	m_overrides[key]       = m_entries.value(key);
	m_comment_overrides.insert(key);
	Save();
}

void CompatibilityDatabase::ApplyOverrides() {
	for (const auto& key: m_status_overrides) {
		m_entries[key].status = m_overrides.value(key).status;
	}
	for (const auto& key: m_comment_overrides) {
		m_entries[key].comment = m_overrides.value(key).comment;
	}
}

void CompatibilityDatabase::Save() const {
	QJsonObject root;
	if (m_local) {
		for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
			root.insert(it.key(),
			            QJsonObject {{QStringLiteral("status"), StatusToText(it.value().status)},
			                         {QStringLiteral("comment"), it.value().comment}});
		}
	} else {
		for (auto it = m_overrides.constBegin(); it != m_overrides.constEnd(); ++it) {
			QJsonObject entry;
			if (m_status_overrides.contains(it.key())) {
				entry.insert(QStringLiteral("status"), StatusToText(it.value().status));
			}
			if (m_comment_overrides.contains(it.key())) {
				entry.insert(QStringLiteral("comment"), it.value().comment);
			}
			root.insert(it.key(), entry);
		}
	}

	const auto file_name = m_local ? FILE_NAME : OVERRIDES_FILE_NAME;
	QSaveFile  file(QDir(".").absoluteFilePath(QString::fromLatin1(file_name)));
	const auto data = QJsonDocument(root).toJson(QJsonDocument::Indented);
	if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
		qWarning() << "Could not save compatibility database:" << file.errorString();
	}
}
