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

#include "search/controller.h"

#include "geo/calculations.h"
#include "search/column.h"
#include "search/columnlist.h"
#include "sql/sqlrecord.h"

#include <QTableView>
#include <QHeaderView>
#include <QSpinBox>
#include <QApplication>

using atools::sql::SqlQuery;
using atools::sql::SqlDatabase;

Controller::Controller(QWidget *parent, atools::sql::SqlDatabase *sqlDb, ColumnList *cols,
                       QTableView *tableView)
  : parentWidget(parent), db(sqlDb), view(tableView), columns(cols)
{
}

Controller::~Controller()
{
  viewSetModel(nullptr);

  if(proxyModel != nullptr)
    proxyModel->clear();
  delete proxyModel;
  proxyModel = nullptr;

  if(model != nullptr)
    model->clear();
  delete model;
  model = nullptr;
}

void Controller::preDatabaseLoad()
{
  viewSetModel(nullptr);

  if(model != nullptr)
    model->clear();
}

void Controller::postDatabaseLoad()
{
  if(proxyModel != nullptr)
    viewSetModel(proxyModel);
  else
    viewSetModel(model);
  model->resetSqlQuery();
  model->fillHeaderData();
}

void Controller::filterIncluding(const QModelIndex& index)
{
  view->clearSelection();
  model->filterIncluding(toSource(index));
  searchParamsChanged = true;
}

void Controller::filterExcluding(const QModelIndex& index)
{
  view->clearSelection();
  model->filterExcluding(toSource(index));
  searchParamsChanged = true;
}

atools::geo::Pos Controller::getGeoPos(const QModelIndex& index)
{
  if(index.isValid())
  {
    QModelIndex localIndex = toSource(index);

    QVariant lon = getRawData(localIndex.row(), "lonx");
    QVariant lat = getRawData(localIndex.row(), "laty");

    if(!lon.isNull() && !lat.isNull())
      return atools::geo::Pos(lon.toFloat(), lat.toFloat());
  }
  return atools::geo::Pos();
}

void Controller::filterByLineEdit(const Column *col, const QString& text)
{
  view->clearSelection();
  model->filter(col, text);
  searchParamsChanged = true;
}

void Controller::filterBySpinBox(const Column *col, int value)
{
  view->clearSelection();
  if(col->getSpinBoxWidget()->value() == col->getSpinBoxWidget()->minimum())
    // Send a null variant if spin box is at minimum value
    model->filter(col, QVariant(QVariant::Int));
  else
    model->filter(col, value);
  searchParamsChanged = true;
}

void Controller::filterByIdent(const QString& ident, const QString& region, const QString& airportIdent)
{
  view->clearSelection();
  model->filterByIdent(ident, region, airportIdent);
  searchParamsChanged = true;
}

void Controller::filterByMinMaxSpinBox(const Column *col, int minValue, int maxValue)
{
  view->clearSelection();
  QVariant minVal(minValue), maxVal(maxValue);
  if(col->getMinSpinBoxWidget()->value() == col->getMinSpinBoxWidget()->minimum())
    // Send a null variant for min if minimum spin box is at minimum value
    minVal = QVariant(QVariant::Int);

  if(col->getMaxSpinBoxWidget()->value() == col->getMaxSpinBoxWidget()->maximum())
    // Send a null variant for max if maximum spin box is at maximum value
    maxVal = QVariant(QVariant::Int);

  model->filter(col, minVal, maxVal);
  searchParamsChanged = true;
}

void Controller::filterByCheckbox(const Column *col, int state, bool triState)
{
  view->clearSelection();
  if(triState)
  {
    Qt::CheckState s = static_cast<Qt::CheckState>(state);
    switch(s)
    {
      case Qt::Unchecked:
        model->filter(col, 0);
        break;
      case Qt::PartiallyChecked:
        // null for partially checked
        model->filter(col, QVariant(QVariant::Int));
        break;
      case Qt::Checked:
        model->filter(col, 1);
        break;
    }
  }
  else
    model->filter(col, state == Qt::Checked ? 1 : QVariant(QVariant::Int));
  searchParamsChanged = true;
}

void Controller::filterByComboBox(const Column *col, int value, bool noFilter)
{
  view->clearSelection();
  if(noFilter)
    // Index 0 for combo box means here: no filter, so remove it and send null variant
    model->filter(col, QVariant(QVariant::Int));
  else
    model->filter(col, value);
  searchParamsChanged = true;
}

void Controller::filterByDistance(const atools::geo::Pos& center, sqlproxymodel::SearchDirection dir,
                                  int minDistance, int maxDistance)
{
  if(center.isValid())
  {
    // Start or update distance search
    view->clearSelection();

    currentDistanceCenter = center;
    atools::geo::Rect rect(center, atools::geo::nmToMeter(maxDistance));

    bool proxyWasNull = false;
    if(proxyModel == nullptr)
    {
      // No proxy - create a new one and assign it to the model
      proxyWasNull = true;
      // Controller takes ownership
      proxyModel = new SqlProxyModel(this, model);
      proxyModel->setDynamicSortFilter(true);
      proxyModel->setSourceModel(model);

      viewSetModel(proxyModel);
    }

    // Update distances in proxy to get precise radius filtering (second filter stage)
    proxyModel->setDistanceFilter(center, dir, minDistance, maxDistance);

    // Update rectangle filter in query model (first coarse filter stage)
    model->filterByBoundingRect(rect);

    if(proxyWasNull)
    {
      // New proxy created so set ordering and more
      proxyModel->sort(0, Qt::DescendingOrder);
      model->setSort("distance", Qt::DescendingOrder);
      model->fillHeaderData();
      view->reset();
      processViewColumns();
    }
  }
  else
  {
    // End distance search
    view->clearSelection();
    currentDistanceCenter = atools::geo::Pos();

    // Set sql model back into view
    viewSetModel(model);

    if(proxyModel != nullptr)
    {
      proxyModel->clear();
      delete proxyModel;
      proxyModel = nullptr;
    }

    model->filterByBoundingRect(atools::geo::Rect());
    model->fillHeaderData();
    processViewColumns();
  }
  searchParamsChanged = true;
}

void Controller::filterByDistanceUpdate(sqlproxymodel::SearchDirection dir, int minDistance, int maxDistance)
{
  if(proxyModel != nullptr)
  {
    view->clearSelection();
    // Create new bounding rectangle for first stage search
    atools::geo::Rect rect(currentDistanceCenter, atools::geo::nmToMeter(maxDistance));

    // Update proxy second stage filter
    proxyModel->setDistanceFilter(currentDistanceCenter, dir, minDistance, maxDistance);
    // Update SQL model coarse first stage filter
    model->filterByBoundingRect(rect);
    searchParamsChanged = true;
  }
}

/* Set new model into view and delete old selection model to avoid memory leak */
void Controller::viewSetModel(QAbstractItemModel *newModel)
{
  QItemSelectionModel *m = view->selectionModel();
  view->setModel(newModel);
  delete m;
}

void Controller::selectAllRows()
{
  Q_ASSERT(view->selectionModel() != nullptr);
  view->selectAll();
}

const QItemSelection Controller::getSelection() const
{
  if(view->selectionModel() != nullptr)
    return view->selectionModel()->selection();
  else
    return QItemSelection();
}

int Controller::getVisibleRowCount() const
{
  if(proxyModel != nullptr)
    // Proxy fine second stage filter knows precise count
    return proxyModel->rowCount();
  else if(model != nullptr)
    return model->rowCount();

  return 0;
}

int Controller::getTotalRowCount() const
{
  if(proxyModel != nullptr)
    // Proxy fine second stage filter knows precise count
    return proxyModel->rowCount();
  else if(model != nullptr)
    return model->getTotalRowCount();
  else
    return 0;
}

bool Controller::isColumnVisibleInView(int physicalIndex) const
{
  return view->columnWidth(physicalIndex) > view->horizontalHeader()->minimumSectionSize() + 1;
}

int Controller::getColumnVisualIndex(int physicalIndex) const
{

  return view->horizontalHeader()->visualIndex(physicalIndex);
}

const Column *Controller::getColumnDescriptor(const QString& colName) const
{
  return model->getColumnModel(colName);
}

const Column *Controller::getColumnDescriptor(int physicalIndex) const
{
  return model->getColumnModel(physicalIndex);
}

void Controller::resetView()
{
  if(columns != nullptr)
    columns->resetWidgets();

  // Reorder columns to match model order
  QHeaderView *header = view->horizontalHeader();
  for(int i = 0; i < header->count(); ++i)
    header->moveSection(header->visualIndex(i), i);

  model->resetView();

  processViewColumns();

  view->resizeColumnsToContents();
  saveTempViewState();
}

void Controller::resetSearch()
{
  if(columns != nullptr)
    // Will also delete proxy by check box message
    columns->resetWidgets();

  if(model != nullptr)
    model->resetSearch();
}

QString Controller::getCurrentSqlQuery() const
{
  return model->getCurrentSqlQuery();
}

QModelIndex Controller::getModelIndexAt(const QPoint& pos) const
{
  return view->indexAt(pos);
}

QString Controller::getFieldDataAt(const QModelIndex& index) const
{
  return model->getFormattedFieldData(toSource(index)).toString();
}

QModelIndex Controller::toSource(const QModelIndex& index) const
{
  if(proxyModel != nullptr)
    return proxyModel->mapToSource(index);
  else
    return index;
}

QModelIndex Controller::fromSource(const QModelIndex& index) const
{
  if(proxyModel != nullptr)
    return proxyModel->mapFromSource(index);
  else
    return index;
}

int Controller::getIdForRow(const QModelIndex& index)
{
  if(index.isValid())
    return model->getRawData(toSource(index).row(), columns->getIdColumnName()).toInt();
  else
    return -1;
}

/* Adapt view to model columns - hide/show, indicate sort, sort, etc. */
void Controller::processViewColumns()
{
  const Column *colDescrCurSort = nullptr;
  atools::sql::SqlRecord rec = model->getSqlRecord();
  int cnt = rec.count();
  for(int i = 0; i < cnt; ++i)
  {
    QString field = rec.fieldName(i);
    const Column *colDescr = columns->getColumn(field);

    if(!isDistanceSearch() && colDescr->isDistance())
      // Hide special distance search columns "distance" and "heading"
      view->hideColumn(i);
    else if(colDescr->isHidden())
      view->hideColumn(i);
    else
      view->showColumn(i);

    // Set sort column
    if(model->getSortColumn().isEmpty())
    {
      if(colDescr->isDefaultSort())
      {
        // Highlight default sort column
        view->sortByColumn(i, colDescr->getDefaultSortOrder());
        colDescrCurSort = colDescr;
      }
    }
    else if(field == model->getSortColumn())
    {
      // Highlight user sort column
      view->sortByColumn(i, model->getSortOrder());
      colDescrCurSort = colDescr;
    }
  }

  // Apply sort order to view
  const Column *colDescrDefSort = columns->getDefaultSortColumn();
  int idx = rec.indexOf(colDescrDefSort->getColumnName());
  if(colDescrCurSort == nullptr)
    // Sort by default
    view->sortByColumn(idx, colDescrDefSort->getDefaultSortOrder());
  else
  {
    if(colDescrCurSort->isHidden())
      view->sortByColumn(idx, colDescrDefSort->getDefaultSortOrder());
    else if(!isDistanceSearch())
      if(colDescrCurSort->isDistance())
        view->sortByColumn(idx, colDescrDefSort->getDefaultSortOrder());
  }
}

void Controller::prepareModel()
{
  model = new SqlModel(parentWidget, db, columns);

  viewSetModel(model);

  model->fillHeaderData();
  processViewColumns();
  restoreViewState();
}

void Controller::saveTempViewState()
{
  viewState = view->horizontalHeader()->saveState();
}

void Controller::restoreViewState()
{
  if(!viewState.isEmpty())
    view->horizontalHeader()->restoreState(viewState);
}

void Controller::loadAllRowsForDistanceSearch()
{
  if(searchParamsChanged && proxyModel != nullptr)
  {
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);

    // Run query again
    model->resetSqlQuery();

    // Let proxy know that filter parameters have changed
    proxyModel->invalidate();

    while(model->canFetchMore())
      // Fetch as long as we can
      model->fetchMore(QModelIndex());

    QGuiApplication::restoreOverrideCursor();
    searchParamsChanged = false;
  }
}

void Controller::setDataCallback(const SqlModel::DataFunctionType& value, const QSet<Qt::ItemDataRole>& roles)
{
  model->setDataCallback(value, roles);
}

void Controller::loadAllRows()
{
  QGuiApplication::setOverrideCursor(Qt::WaitCursor);

  if(proxyModel != nullptr)
  {
    // Run query again
    model->resetSqlQuery();

    // Let proxy know that filter parameters have changed
    proxyModel->invalidate();
  }

  while(model->canFetchMore())
    model->fetchMore(QModelIndex());

  QGuiApplication::restoreOverrideCursor();
}

QVector<const Column *> Controller::getCurrentColumns() const
{
  QVector<const Column *> cols;
  atools::sql::SqlRecord rec = model->getSqlRecord();
  for(int i = 0; i < rec.count(); ++i)
    cols.append(columns->getColumn(rec.fieldName(i)));
  return cols;
}

void Controller::initRecord(atools::sql::SqlRecord& rec)
{
  atools::sql::SqlRecord from = model->getSqlRecord();
  for(int i = 0; i < from.count(); i++)
    rec.appendField(from.fieldName(i), from.fieldType(i));
}

void Controller::fillRecord(int row, atools::sql::SqlRecord& rec)
{
  int srow = row;
  if(proxyModel != nullptr)
    srow = toSource(proxyModel->index(row, 0)).row();

  for(int i = 0; i < rec.count(); i++)
    rec.setValue(i, model->getRawData(srow, i));
}

QVariant Controller::getRawData(int row, const QString& colname) const
{
  int srow = row;
  if(proxyModel != nullptr)
    srow = toSource(proxyModel->index(row, 0)).row();

  int colIdx = model->getSqlRecord().indexOf(colname);
  return model->getRawData(srow, colIdx);
}

QVariant Controller::getRawData(int row, int col) const
{
  int srow = row;
  if(proxyModel != nullptr)
    srow = toSource(proxyModel->index(row, 0)).row();
  return model->getRawData(srow, col);
}

QString Controller::getSortColumn() const
{
  return model->getSortColumn();
}

int Controller::getSortColumnIndex() const
{
  return model->getSortColumnIndex();
}
