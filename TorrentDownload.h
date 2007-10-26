#ifndef TORRENTDOWNLOAD_H
#define TORRENTDOWNLOAD_H
#include "Transfer.h"
#include <QTimer>
#include <QMutex>
#include <libtorrent/session.hpp>
#include <libtorrent/torrent_handle.hpp>

class TorrentWorker;

class TorrentDownload : public Transfer
{
Q_OBJECT
public:
	TorrentDownload();
	virtual ~TorrentDownload();
	
	static Transfer* createInstance() { return new TorrentDownload; }
	static WidgetHostChild* createSettingsWidget(QWidget* w,QIcon& i);
	static int acceptable(QString url);
	
	static void globalInit();
	static void applySettings();
	static void globalExit();
	
	static QByteArray bencode_simple(libtorrent::entry e);
	static QString bencode(libtorrent::entry e);
	static libtorrent::entry bdecode_simple(QByteArray d);
	static libtorrent::entry bdecode(QString d);
	
	virtual void init(QString source, QString target);
	virtual void setObject(QString source);
	
	virtual void changeActive(bool nowActive);
	virtual void setSpeedLimits(int down, int up);
	
	virtual QString object() const;
	virtual QString myClass() const { return "TorrentDownload"; }
	virtual QString name() const;
	virtual QString message() const;
	virtual Mode primaryMode() { return Download; }
	virtual void speeds(int& down, int& up) const;
	virtual qulonglong total() const;
	virtual qulonglong done() const;
	
	virtual void load(const QDomNode& map);
	virtual void save(QDomDocument& doc, QDomNode& map);
	
	qint64 totalDownload() { return m_nPrevDownload + m_status.total_download; }
	qint64 totalUpload() { return m_nPrevUpload + m_status.total_upload; }
private slots:
	void fileStateChanged(Transfer::State,Transfer::State);
	void forceReannounce();
protected:
	libtorrent::torrent_handle m_handle;
	libtorrent::torrent_info m_info;
	libtorrent::torrent_status m_status;
	
	QString m_strError, m_strTarget;
	qint64 m_nPrevDownload, m_nPrevUpload;
	
	Transfer* m_pFileDownload;
	
	static libtorrent::session* m_session;
	static TorrentWorker* m_worker;
	
	friend class TorrentWorker;
};

class TorrentWorker : public QObject
{
Q_OBJECT
public:
	TorrentWorker();
	void addObject(TorrentDownload* d);
	void removeObject(TorrentDownload* d);
	TorrentDownload* getByHandle(libtorrent::torrent_handle handle) const;
public slots:
	void doWork();
private:
	QTimer m_timer;
	QMutex m_mutex;
	QList<TorrentDownload*> m_objects;
};

#endif
