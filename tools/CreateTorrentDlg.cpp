#include "CreateTorrentDlg.h"
#include "fatrat.h"
#include <cmath>
#include <fstream>
#include <QFileDialog>
#include <QFile>
#include <QDir>
#include <QMessageBox>
#include <QPushButton>
#include <QFileInfo>
#include <libtorrent/bencode.hpp>
#include <libtorrent/storage.hpp>
#include <libtorrent/hasher.hpp>
#include <libtorrent/file_pool.hpp>

CreateTorrentDlg::CreateTorrentDlg(QWidget* parent)
	: QDialog(parent), m_hasher(0)
{
	setupUi(this);
	
	pushCreate = buttonBox->addButton(tr("Create"), QDialogButtonBox::ActionRole);
	buttonBox->addButton(QDialogButtonBox::Cancel);
	
	QStringList sizes;
	for(long s=64*1024;s <= 8*1024*1024;s*=2)
		sizes << formatSize(s, false);
	comboPieceSize->addItems(sizes);
	comboPieceSize->setCurrentIndex(3);
	
	connect(toolDataF, SIGNAL(clicked()), this, SLOT(browseFiles()));
	connect(toolDataD, SIGNAL(clicked()), this, SLOT(browseDirs()));
	
	connect(pushCreate, SIGNAL(clicked()), this, SLOT(createTorrent()));
	
	progressBar->setVisible(false);
}

QWidget* CreateTorrentDlg::create()
{
	return new CreateTorrentDlg(getMainWindow());
}

void CreateTorrentDlg::browseFiles()
{
	QString r = QFileDialog::getOpenFileName(this, "FatRat", lineData->text());
	if(!r.isEmpty())
		lineData->setText(r);
}

void CreateTorrentDlg::browseDirs()
{
	QString r = QFileDialog::getExistingDirectory(this, "FatRat", lineData->text());
	if(!r.isEmpty())
		lineData->setText(r);
}

void CreateTorrentDlg::createTorrent()
{
	bool bDataInvalid = false, bDirectory = false, bPrivate;
	QString data = lineData->text();
	
	if(data.isEmpty())
		bDataInvalid = true;
	else
	{
		QFileInfo info(data);
		if(info.exists())
			bDirectory = info.isDir();
		else
			bDataInvalid = true;
	}
	
	if(bDataInvalid)
	{
		QMessageBox::critical(this, "FatRat", tr("The data path is invalid."));
		return;
	}
	
	bPrivate = checkPrivate->isChecked();
	
	libtorrent::torrent_info* info = new libtorrent::torrent_info;
	QByteArray comment = lineComment->text().toUtf8();
	QList<QPair<QString, qint64> > files;
	
	info->set_creator("FatRat " VERSION);
	info->set_comment(comment.data());
	info->set_piece_size(64*1024 * pow(2, comboPieceSize->currentIndex()));
	
	for(int i=0;i<listTrackers->count();i++)
	{
		QByteArray text = listTrackers->item(i)->text().toUtf8();
		
		if((text.startsWith("http://") || text.startsWith("udp://")) && text.size() > 7)
			info->add_tracker(text.data());
	}
	
	info->set_priv(bPrivate);
	
	for(int i=0;bPrivate && i<listNodes->count();i++)
	{
		QByteArray text = listNodes->item(i)->text().toUtf8();
		if(!text.contains(':'))
			continue;
		
		std::string addr;
		int port;
		
		if(text[0] == '[') // IPv6
		{
			int pos = text.lastIndexOf("]:");
			if(pos < 0)
				continue;
			port = atoi(text.data()+pos+2);
			addr = std::string(text.data()+1, pos-1);
		}
		else
		{
			int pos = text.lastIndexOf(':');
			port = atoi(text.data()+pos+1);
			addr = std::string(text.data(), pos);
		}
		
		info->add_node(std::pair<std::string, int>(addr, port));
	}
	
	for(int i=0;i<listWebSeeds->count();i++)
	{
		QByteArray text = listWebSeeds->item(i)->text().toUtf8();
		if(!text.startsWith("http://"))
			continue;
		
		info->add_url_seed(text.data());
	}
	
	QByteArray baseDir;
	if(!bDirectory)
	{
		QFileInfo file(data);
		
		baseDir = file.absolutePath().toUtf8();
		files << QPair<QString, qint64>(file.fileName(), file.size());
	}
	else
	{
		QDir dir(data);
		baseDir = dir.dirName().toUtf8() + '/';
		
		recurseDir(files, baseDir, data);
		dir.cdUp();
		
		baseDir = dir.absolutePath().toUtf8();
	}
	
	for(int i=0;i<files.size();i++)
	{
		QByteArray name = files[i].first.toUtf8();
		qDebug() << name;
		info->add_file(name.data(), files[i].second);
	}
	
	m_hasher = new HasherThread(baseDir, info, this);
	progressBar->setVisible(true);
	progressBar->setMaximum(info->num_pieces());
	pushCreate->setDisabled(true);
	
	connect(m_hasher, SIGNAL(progress(int)), progressBar, SLOT(setValue(int)));
	connect(m_hasher, SIGNAL(finished()), this, SLOT(hasherFinished()));
	m_hasher->start();
}

void CreateTorrentDlg::recurseDir(QList<QPair<QString, qint64> >& list, QString prefix, QString path)
{
	QDir dir(path);
	QFileInfoList flist = dir.entryInfoList();
	
	for(int i=0;i<flist.size();i++)
	{
		if(flist[i].fileName() == "." || flist[i].fileName() == "..")
			continue;
		
		if(flist[i].isDir())
			recurseDir(list, prefix + flist[i].fileName() + '/', flist[i].absoluteFilePath());
		else
			list << QPair<QString, qint64>(prefix + flist[i].fileName(), flist[i].size());
	}
}

void CreateTorrentDlg::hasherFinished()
{
	libtorrent::torrent_info* info = m_hasher->info();
	QByteArray torrent;
	
	progressBar->setVisible(false);
	pushCreate->setDisabled(false);
	
	if(m_hasher->error().isEmpty())
	{
		torrent = QFileDialog::getSaveFileName(this, "FatRat", QString(), tr("Torrents (*.torrent)")).toUtf8();
		if(!torrent.isEmpty())
		{
			libtorrent::entry e = info->create_torrent();
			std::ofstream fout(torrent.constData(), std::ios_base::binary);
			libtorrent::bencode(std::ostream_iterator<char>(fout), e);
		}
		close();
	}
	else
	{
		QMessageBox::critical(this, "FatRat", m_hasher->error());
	}
	
	delete info;
	delete m_hasher;
	m_hasher = 0;
}

HasherThread::HasherThread(QByteArray baseDir, libtorrent::torrent_info* info, QObject* parent)
	: QThread(parent), m_baseDir(baseDir), m_info(info), m_bAbort(false)
{
}

HasherThread::~HasherThread()
{
	m_bAbort = true;
	wait();
}

void HasherThread::run()
{
	int num = m_info->num_pieces();
	char* buf = new char[m_info->piece_length()];
	
	try
	{
		libtorrent::file_pool fp;
		libtorrent::storage_interface* st = libtorrent::default_storage_constructor(m_info, m_baseDir.data(), fp);
		
		for(int i=0;i<num && !m_bAbort;i++)
		{
			st->read(buf, i, 0, m_info->piece_size(i));
			libtorrent::hasher h(buf, m_info->piece_size(i));
			m_info->set_hash(i, h.final());
			
			emit progress(i);
		}
	}
	catch(const std::exception& e)
	{
		std::string err = e.what();
		m_strError = QString::fromUtf8(err.c_str());
	}
	
	//delete st;
	delete [] buf;
}
