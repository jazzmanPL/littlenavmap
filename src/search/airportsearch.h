/*****************************************************************************
* Copyright 2015-2016 Alexander Barthel albar965@mailbox.org
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
*****************************************************************************/

#ifndef LITTLENAVMAP_AIRPORTSEARCH_H
#define LITTLENAVMAP_AIRPORTSEARCH_H

#include "search/searchbase.h"

#include <QObject>

class Column;
class AirportIconDelegate;
class QAction;

namespace atools {
namespace sql {
class SqlDatabase;
}
}

/*
 * Airport search tab including all search widgets and the result table view.
 */
class AirportSearch :
  public SearchBase
{
  Q_OBJECT

public:
  AirportSearch(MainWindow *parent, QTableView *tableView,
                MapQuery *query, int tabWidgetIndex);
  virtual ~AirportSearch();

  /* All state saving is done through the widget state */
  virtual void saveState() override;
  virtual void restoreState() override;

  virtual void getSelectedMapObjects(maptypes::MapSearchResult& result) const override;
  virtual void connectSearchSlots() override;
  virtual void postDatabaseLoad() override;

private:
  virtual void updateButtonMenu() override;

  void setCallbacks();
  QVariant modelDataHandler(int colIndex, int rowIndex, const Column *col, const QVariant& roleValue,
                            const QVariant& displayRoleValue, Qt::ItemDataRole role) const;
  QString formatModelData(const Column *col, const QVariant& displayRoleValue) const;

  static const QSet<QString> NUMBER_COLUMNS;

  /* All layouts, lines and drop down menu items */
  QList<QObject *> airportSearchWidgets;

  /* All drop down menu actions */
  QList<QAction *> airportSearchMenuActions;

  /* Draw airport icon into ident table column */
  AirportIconDelegate *iconDelegate = nullptr;

};

#endif // LITTLENAVMAP_AIRPORTSEARCH_H
