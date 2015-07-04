/*
 * Cantata
 *
 * Copyright (c) 2011-2014 Craig Drummond <craig.p.drummond@gmail.com>
 *
 * ----
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "devicespage.h"
#include "models/musiclibraryitemroot.h"
#include "models/musiclibraryitemartist.h"
#include "models/musiclibraryitemalbum.h"
#include "models/musiclibraryitemsong.h"
#include "models/devicesmodel.h"
#include "gui/settings.h"
#include "support/messagebox.h"
#include "support/localize.h"
#include "support/configuration.h"
#include "widgets/icons.h"
#include "widgets/menubutton.h"
#include "support/action.h"
#include "gui/stdactions.h"
#ifdef ENABLE_REMOTE_DEVICES
#include "remotedevicepropertiesdialog.h"
#include "devicepropertieswidget.h"
#endif
#ifdef ENABLE_REPLAYGAIN_SUPPORT
#include "replaygain/rgdialog.h"
#endif
#include "tags/tageditor.h"
#include "actiondialog.h"
#include "tags/trackorganiser.h"
#include "gui/preferencesdialog.h"
#include "gui/coverdialog.h"
#include "mpd-interface/mpdconnection.h"
#if defined CDDB_FOUND || defined MUSICBRAINZ5_FOUND
#include "audiocddevice.h"
#include "albumdetailsdialog.h"
#include "cddbselectiondialog.h"
#endif
#include <QMenu>

DevicesPage::DevicesPage(QWidget *p)
    : SinglePageWidget(p)
{
    copyAction = new Action(Icons::self()->importIcon, i18n("Copy To Library"), this);
    ToolButton *copyToLibraryButton=new ToolButton(this);
    copyToLibraryButton->setDefaultAction(copyAction);
    #ifdef ENABLE_REMOTE_DEVICES
    forgetDeviceAction=new Action(Icon("list-remove"), i18n("Forget Device"), this);
    connect(forgetDeviceAction, SIGNAL(triggered()), this, SLOT(forgetRemoteDevice()));
    #endif
    connect(DevicesModel::self()->connectAct(), SIGNAL(triggered()), this, SLOT(toggleDevice()));
    connect(DevicesModel::self()->disconnectAct(), SIGNAL(triggered()), this, SLOT(toggleDevice()));
    connect(this, SIGNAL(add(const QStringList &, bool, quint8)), MPDConnection::self(), SLOT(add(const QStringList &, bool, quint8)));
    connect(this, SIGNAL(addSongsToPlaylist(const QString &, const QStringList &)), MPDConnection::self(), SLOT(addToPlaylist(const QString &, const QStringList &)));
    connect(DevicesModel::self(), SIGNAL(updated(QModelIndex)), this, SLOT(updated(QModelIndex)));
    connect(view, SIGNAL(doubleClicked(const QModelIndex &)), this, SLOT(itemDoubleClicked(const QModelIndex &)));
    connect(view, SIGNAL(searchItems()), this, SLOT(searchItems()));
    connect(view, SIGNAL(itemsSelected(bool)), SLOT(controlActions()));
    connect(copyAction, SIGNAL(triggered()), this, SLOT(copyToLibrary()));
    connect(DevicesModel::self()->configureAct(), SIGNAL(triggered()), this, SLOT(configureDevice()));
    connect(DevicesModel::self()->refreshAct(), SIGNAL(triggered()), this, SLOT(refreshDevice()));
    #if defined CDDB_FOUND || defined MUSICBRAINZ5_FOUND
    connect(DevicesModel::self()->editAct(), SIGNAL(triggered()), this, SLOT(editDetails()));
    connect(DevicesModel::self(), SIGNAL(matches(const QString &, const QList<CdAlbum> &)),
            SLOT(cdMatches(const QString &, const QList<CdAlbum> &)));
    #endif
    proxy.setSourceModel(DevicesModel::self());
    view->setModel(&proxy);
    view->setRootIsDecorated(false);
    view->setSearchResetLevel(1);
    Configuration config(metaObject()->className());
    view->load(config);
    MenuButton *menu=new MenuButton(this);
    menu->addAction(createViewMenu(QList<ItemView::Mode>() << ItemView::Mode_BasicTree << ItemView::Mode_SimpleTree
                                                           << ItemView::Mode_DetailedTree << ItemView::Mode_List));
    menu->addSeparator();
    menu->addAction(DevicesModel::self()->configureAct());
    menu->addAction(DevicesModel::self()->refreshAct());
    #ifdef ENABLE_REMOTE_DEVICES
    menu->addSeparator();
    Action *addRemote=new Action(Icon("network-server"), i18n("Add Device"), this);
    connect(addRemote, SIGNAL(triggered()), this, SLOT(addRemoteDevice()));
    menu->addAction(addRemote);
    menu->addAction(forgetDeviceAction);
    #endif
    init(ReplacePlayQueue|AddToPlayQueue, QList<QWidget *>() << menu, QList<QWidget *>() << copyToLibraryButton);

    view->addAction(copyAction);
    view->addAction(StdActions::self()->organiseFilesAction);
    view->addAction(StdActions::self()->editTagsAction);
    #ifdef ENABLE_REPLAYGAIN_SUPPORT
    view->addAction(StdActions::self()->replaygainAction);
    #endif
    #ifdef ENABLE_REMOTE_DEVICES
    view->addSeparator();
    view->addAction(forgetDeviceAction);
    #endif
    view->addSeparator();
    view->addAction(StdActions::self()->deleteSongsAction);
}

DevicesPage::~DevicesPage()
{
    Configuration config(metaObject()->className());
    view->save(config);
}

void DevicesPage::clear()
{
    DevicesModel::self()->clear();
    view->goToTop();
}

QString DevicesPage::activeFsDeviceUdi() const
{
    Device *dev=activeFsDevice();
    return dev ? dev->id() : QString();
}

Device * DevicesPage::activeFsDevice() const
{
    const QModelIndexList selected = view->selectedIndexes(false); // Dont need sorted selection here...

    if (selected.isEmpty()) {
        return 0;
    }

    QString udi;
    Device *activeDev=0;
    foreach (const QModelIndex &idx, selected) {
        QModelIndex index = proxy.mapToSource(idx);
        MusicLibraryItem *item=static_cast<MusicLibraryItem *>(index.internalPointer());

        if (item && MusicLibraryItem::Type_Root!=item->itemType()) {
            while(item->parentItem()) {
                item=item->parentItem();
            }
        }

        if (item && MusicLibraryItem::Type_Root==item->itemType()) {
            Device *dev=static_cast<Device *>(item);
            if (Device::Ums!=dev->devType() && Device::RemoteFs!=dev->devType()) {
                return 0;
            }
            if (activeDev) {
                return 0;
            }
            activeDev=dev;
        }
    }

    return activeDev;
}

QStringList DevicesPage::playableUrls() const
{
    QModelIndexList selected = view->selectedIndexes();
    if (selected.isEmpty()) {
        return QStringList();
    }

    QModelIndexList mapped;
    foreach (const QModelIndex &idx, selected) {
        mapped.append(proxy.mapToSource(idx));
    }

    return DevicesModel::self()->playableUrls(mapped);
}

QList<Song> DevicesPage::selectedSongs(bool allowPlaylists) const
{
    Q_UNUSED(allowPlaylists)
    QModelIndexList selected = view->selectedIndexes();
    if (selected.isEmpty()) {
        return QList<Song>();
    }

    // Ensure all songs are from UMS/Remote devices...
    foreach (const QModelIndex &idx, selected) {
        MusicLibraryItem *item=static_cast<MusicLibraryItem *>(proxy.mapToSource(idx).internalPointer());
        if (item && MusicLibraryItem::Type_Root!=item->itemType()) {
            while(item->parentItem()) {
                item=item->parentItem();
            }
        }

        if (item && MusicLibraryItem::Type_Root==item->itemType()) {
            Device *dev=static_cast<Device *>(item);
            if (Device::Ums!=dev->devType() && Device::RemoteFs!=dev->devType()) {
                return QList<Song>();
            }
        }
    }

    return DevicesModel::self()->songs(proxy.mapToSource(selected, proxy.enabled() && Settings::self()->filteredOnly()));
}

void DevicesPage::addSelectionToPlaylist(const QString &name, bool replace, quint8 priorty)
{
    QStringList files=playableUrls();
    if (!files.isEmpty()) {
        if (name.isEmpty()) {
            emit add(files, replace, priorty);
        } else {
            emit addSongsToPlaylist(name, files);
        }
        view->clearSelection();
    }
}

void DevicesPage::refresh()
{
    view->goToTop();
    DevicesModel::self()->resetModel();
    if (ItemView::Mode_SimpleTree==view->viewMode() || ItemView::Mode_DetailedTree==view->viewMode()) {
        for (int i=0; i<DevicesModel::self()->rowCount(QModelIndex()); ++i) {
            view->setExpanded(proxy.mapFromSource(DevicesModel::self()->index(i, 0, QModelIndex())));
        }
    }
}

void DevicesPage::itemDoubleClicked(const QModelIndex &)
{
//     const QModelIndexList selected = view->selectedIndexes();
//     if (1!=selected.size()) {
//         return; //doubleclick should only have one selected item
//     }
//     MusicDevicesItem *item = DevicesModel::self()->toItem(proxy.mapToSource(selected.at(0)));
//     if (MusicDevicesItem::Type_Song==item->itemType()) {
//         addSelectionToPlaylist();
//     }
}

void DevicesPage::searchItems()
{
    QString text=view->searchText().trimmed();
    proxy.update(text);
    if (proxy.enabled() && !proxy.filterText().isEmpty()) {
        view->expandAll();
    }
}

void DevicesPage::controlActions()
{
    QModelIndexList selected=view->selectedIndexes(false); // Dont need sorted selection here...
    bool haveTracks=false;
    bool onlyFs=true;
    bool singleUdi=true;
//    bool connected=false;
    #ifdef ENABLE_REMOTE_DEVICES
    bool remoteDev=false;
    #endif
    bool deviceSelected=false;
    bool busyDevice=false;
    bool audioCd=false;
    bool canPlay=false;
    QString udi;

    foreach (const QModelIndex &idx, selected) {
        MusicLibraryItem *item=static_cast<MusicLibraryItem *>(proxy.mapToSource(idx).internalPointer());

        if (item && MusicLibraryItem::Type_Root==item->itemType()) {
            deviceSelected=true;
        }

        if (item && MusicLibraryItem::Type_Root!=item->itemType()) {
            while(item->parentItem()) {
                item=item->parentItem();
            }
        }

        if (item && MusicLibraryItem::Type_Root==item->itemType()) {
            Device *dev=static_cast<Device *>(item);
            if (!dev->isStdFs()) {
                onlyFs=false;
            }
            if (!dev->isIdle()) {
                busyDevice=true;
            }
            if (Device::AudioCd==dev->devType()) {
                audioCd=true;
            }
            #ifdef ENABLE_REMOTE_DEVICES
            else if (Device::RemoteFs==dev->devType()) {
                remoteDev=true;
            }
            #endif
            canPlay=dev->canPlaySongs();
            if (udi.isEmpty()) {
                udi=dev->id();
            } else if (udi!=dev->id()) {
                singleUdi=false;
            }
            if (!haveTracks) {
                haveTracks=dev->childCount()>0;
            }
//            connected=dev->isConnected();
        }
    }

    DevicesModel::self()->configureAct()->setEnabled(!busyDevice && 1==selected.count() && !audioCd);
    DevicesModel::self()->refreshAct()->setEnabled(!busyDevice && 1==selected.count());
    copyAction->setEnabled(!busyDevice && haveTracks && (!deviceSelected || audioCd));
    StdActions::self()->deleteSongsAction->setEnabled(!audioCd && !busyDevice && haveTracks && !deviceSelected);
    StdActions::self()->editTagsAction->setEnabled(!busyDevice && haveTracks && onlyFs && singleUdi && !deviceSelected);
    #ifdef ENABLE_REPLAYGAIN_SUPPORT
    StdActions::self()->replaygainAction->setEnabled(!busyDevice && haveTracks && onlyFs && singleUdi && !deviceSelected);
    #endif
    StdActions::self()->organiseFilesAction->setEnabled(!busyDevice && haveTracks && onlyFs && singleUdi && !deviceSelected);
    StdActions::self()->addToPlayQueueAction->setEnabled(canPlay && !selected.isEmpty() && singleUdi && !busyDevice && haveTracks && (audioCd || !deviceSelected));
    StdActions::self()->addWithPriorityAction->setEnabled(StdActions::self()->addToPlayQueueAction->isEnabled());
    StdActions::self()->replacePlayQueueAction->setEnabled(StdActions::self()->addToPlayQueueAction->isEnabled());
    #ifdef ENABLE_REMOTE_DEVICES
    forgetDeviceAction->setEnabled(singleUdi && remoteDev);
    #endif
    #if defined CDDB_FOUND || defined MUSICBRAINZ5_FOUND
    DevicesModel::self()->editAct()->setEnabled(!AlbumDetailsDialog::instanceCount() && !busyDevice && 1==selected.count() && audioCd && haveTracks && deviceSelected);
    #endif
}

void DevicesPage::copyToLibrary()
{
    const QModelIndexList selected = view->selectedIndexes();

    if (selected.isEmpty()) {
        return;
    }

    QModelIndexList mapped;
    foreach (const QModelIndex &idx, selected) {
        mapped.append(proxy.mapToSource(idx));
    }

    MusicLibraryItem *item=static_cast<MusicLibraryItem *>(mapped.first().internalPointer());
    while (item->parentItem()) {
        item=item->parentItem();
    }
    QString udi;
    if (MusicLibraryItem::Type_Root==item->itemType()) {
        udi=static_cast<Device *>(item)->id();
    }

    if (udi.isEmpty()) {
        return;
    }

    QList<Song> songs=DevicesModel::self()->songs(mapped);

    if (!songs.isEmpty()) {
        emit addToDevice(udi, QString(), songs);
        view->clearSelection();
    }
}

void DevicesPage::configureDevice()
{
    const QModelIndexList selected = view->selectedIndexes(false); // Dont need sorted selection here...

    if (1!=selected.size()) {
        return;
    }

    MusicLibraryItem *item=static_cast<MusicLibraryItem *>(proxy.mapToSource(selected.first()).internalPointer());

    if (MusicLibraryItem::Type_Root==item->itemType()) {
        static_cast<Device *>(item)->configure(this);
    }
}

void DevicesPage::refreshDevice()
{
    const QModelIndexList selected = view->selectedIndexes(false); // Dont need sorted selection here...

    if (1!=selected.size()) {
        return;
    }

    MusicLibraryItem *item=static_cast<MusicLibraryItem *>(proxy.mapToSource(selected.first()).internalPointer());

    if (MusicLibraryItem::Type_Root==item->itemType()) {
        Device *dev=static_cast<Device *>(item);
        QString udi=dev->id();
        bool full=true;

        if (Device::AudioCd==dev->devType()) {
            // Bit hacky - we use 'full' to determine CDDB/MusicBrainz!
            #if defined CDDB_FOUND && defined MUSICBRAINZ5_FOUND
            switch (MessageBox::questionYesNoCancel(this, i18n("Lookup album and track details?"),
                                                    i18n("Refresh"), GuiItem(i18n("Via CDDB")), GuiItem(i18n("Via MusicBrainz")))) {
            case MessageBox::Yes:
                full=true; // full==true => CDDB
                break;
            case MessageBox::No:
                full=false; // full==false => MusicBrainz
                break;
            default:
                return;
            }
            #else
            if (MessageBox::No==MessageBox::questionYesNo(this, i18n("Lookup album and track details?"),
                                                          i18n("Refresh"), GuiItem(i18n("Refresh")), StdGuiItem::cancel())) {
                return;
            }
            #endif
            dev=DevicesModel::self()->device(udi);
        } else {
            if (dev->childCount() && Device::Mtp!=dev->devType()) {
                static const QChar constBullet(0x2022);

                switch (MessageBox::questionYesNoCancel(this, i18n("Which type of refresh do you wish to perform?")+QLatin1String("\n\n")+
                                                              constBullet+QLatin1Char(' ')+i18n("Partial - Only new songs are scanned (quick)")+QLatin1Char('\n')+
                                                              constBullet+QLatin1Char(' ')+i18n("Full - All songs are rescanned (slow)"),
                                                        i18n("Refresh"), GuiItem(i18n("Partial")), GuiItem(i18n("Full")))) {
                case MessageBox::Yes:
                    full=false;
                case MessageBox::No:
                    break;
                default:
                    return;
                }
                // We have to query for device again, as it could have been unplugged whilst the above question dialog was visible...
                dev=DevicesModel::self()->device(udi);
            }
        }
        if (dev) {
            dev->rescan(full);
        }
    }
}

void DevicesPage::deleteSongs()
{
    const QModelIndexList selected = view->selectedIndexes();

    if (selected.isEmpty()) {
        return;
    }

    QModelIndexList mapped;
    foreach (const QModelIndex &idx, selected) {
        mapped.append(proxy.mapToSource(idx));
    }

    MusicLibraryItem *item=static_cast<MusicLibraryItem *>(mapped.first().internalPointer());
    while (item->parentItem()) {
        item=item->parentItem();
    }
    QString udi;
    if (MusicLibraryItem::Type_Root==item->itemType()) {
        udi=static_cast<Device *>(item)->id();
    }

    if (udi.isEmpty()) {
        return;
    }

    QList<Song> songs=DevicesModel::self()->songs(mapped);

    if (!songs.isEmpty()) {
        if (MessageBox::Yes==MessageBox::warningYesNo(this, i18n("Are you sure you wish to delete the selected songs?\n\nThis cannot be undone."),
                                                      i18n("Delete Songs"), StdGuiItem::del(), StdGuiItem::cancel())) {
            emit deleteSongs(udi, songs);
        }
        view->clearSelection();
    }
}

void DevicesPage::addRemoteDevice()
{
    #ifdef ENABLE_REMOTE_DEVICES
    RemoteDevicePropertiesDialog *dlg=new RemoteDevicePropertiesDialog(this);
    dlg->show(DeviceOptions(QLatin1String("cover.jpg")), RemoteFsDevice::Details(), DevicePropertiesWidget::Prop_All-DevicePropertiesWidget::Prop_Folder, true);
    connect(dlg, SIGNAL(updatedSettings(const DeviceOptions &, RemoteFsDevice::Details)),
            DevicesModel::self(), SLOT(addRemoteDevice(const DeviceOptions &, RemoteFsDevice::Details)));
    #endif
}

void DevicesPage::forgetRemoteDevice()
{
    #ifdef ENABLE_REMOTE_DEVICES
    Device *dev=activeFsDevice();
    if (!dev) {
        return;
    }
    QString udi=dev->id();
    QString devName=dev->data();
    if (MessageBox::Yes==MessageBox::warningYesNo(this, i18n("Are you sure you wish to forget '%1'?", devName))) {
        DevicesModel::self()->removeRemoteDevice(udi);
    }
    #endif
}

void DevicesPage::toggleDevice()
{
    const QModelIndexList selected = view->selectedIndexes(false); // Dont need sorted selection here...
    if (1!=selected.size()) {
        return;
    }

    MusicLibraryItem *item=static_cast<MusicLibraryItem *>(proxy.mapToSource(selected.first()).internalPointer());

    if (MusicLibraryItem::Type_Root==item->itemType()) {
        Device *dev=static_cast<Device *>(item);
        if (dev->isConnected() &&
            (Device::AudioCd==dev->devType()
                ? MessageBox::No==MessageBox::warningYesNo(this, i18n("Are you sure you wish to eject Audio CD '%1 - %2'?", dev->data(), dev->subText()),
                                                           i18n("Eject"), GuiItem(i18n("Eject")), StdGuiItem::cancel())
                : MessageBox::No==MessageBox::warningYesNo(this, i18n("Are you sure you wish to disconnect '%1'?", dev->data()),
                                                           i18n("Disconnect"), GuiItem(i18n("Disconnect")), StdGuiItem::cancel()))) {
            return;
        }
        static_cast<Device *>(item)->toggle();
    }
}

void DevicesPage::updated(const QModelIndex &idx)
{
    view->setExpanded(proxy.mapFromSource(idx));
}

void DevicesPage::cdMatches(const QString &udi, const QList<CdAlbum> &albums)
{
    #if defined CDDB_FOUND || defined MUSICBRAINZ5_FOUND
    int chosen=0;
    Device *dev=DevicesModel::self()->device(udi);
    if (dev && Device::AudioCd==dev->devType()) {
        if (Device::AudioCd==dev->devType()) {
            CddbSelectionDialog *dlg=new CddbSelectionDialog(this);
            chosen=dlg->select(albums);
            if (chosen<0 || chosen>=albums.count()) {
                chosen=0;
            }
        }
    }

    // Need to get device again, as it may have been removed!
    dev=DevicesModel::self()->device(udi);
    if (dev && Device::AudioCd==dev->devType()) {
        if (Device::AudioCd==dev->devType()) {
            static_cast<AudioCdDevice *>(dev)->setDetails(albums.at(chosen));
        }
    }
    #else
    Q_UNUSED(udi)
    Q_UNUSED(albums)
    #endif
}

void DevicesPage::editDetails()
{
    #if defined CDDB_FOUND || defined MUSICBRAINZ5_FOUND
    if (AlbumDetailsDialog::instanceCount()) {
        return;
    }
    const QModelIndexList selected = view->selectedIndexes(false); // Dont need sorted selection here...
    if (1!=selected.size()) {
        return;
    }

    MusicLibraryItem *item=static_cast<MusicLibraryItem *>(proxy.mapToSource(selected.first()).internalPointer());

    if (MusicLibraryItem::Type_Root==item->itemType()) {
        Device *dev=static_cast<Device *>(item);
        if (Device::AudioCd==dev->devType()) {
            AlbumDetailsDialog *dlg=new AlbumDetailsDialog(this);
            dlg->show(static_cast<AudioCdDevice *>(dev));
        }
    }
    #endif
}
