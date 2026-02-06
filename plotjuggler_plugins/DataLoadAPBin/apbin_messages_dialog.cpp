#include "apbin_messages_dialog.h"
#include "ui_apbin_messages_dialog.h"

#include <QTableWidget>
#include <QSettings>
#include <QHeaderView>

APBinMessagesDialog::APBinMessagesDialog(const std::vector<APBinMessage>& messages,
                                         const std::map<std::string, float>& parameters,
                                         QWidget* parent)
  : QDialog(parent), ui(new Ui::APBinMessagesDialog)
{
  ui->setupUi(this);
  QTableWidget* table_params = ui->tableWidgetParams;
  QTableWidget* table_messages = ui->tableWidgetMessages;

  // Populate parameters table
  table_params->setRowCount(parameters.size());
  int row = 0;
  for (const auto& param : parameters)
  {
    table_params->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(param.first)));
    table_params->setItem(row, 1, new QTableWidgetItem(QString::number(param.second)));
    row++;
  }
  table_params->sortItems(0);

  // Populate messages table
  table_messages->setRowCount(messages.size());
  row = 0;
  for (const auto& msg : messages)
  {
    // Convert timestamp from microseconds to seconds with 2 decimal places
    QString time = QString::number(msg.timestamp_us / 1000000.0, 'f', 2);
    table_messages->setItem(row, 0, new QTableWidgetItem(time));
    table_messages->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(msg.message)));
    row++;
  }

  // Resize first column to fit content
  table_messages->resizeColumnToContents(0);
}

void APBinMessagesDialog::restoreSettings()
{
  QTableWidget* table_params = ui->tableWidgetParams;
  QTableWidget* table_messages = ui->tableWidgetMessages;

  QSettings settings;
  restoreGeometry(settings.value("APBinMessagesDialog/geometry").toByteArray());
  table_params->horizontalHeader()->restoreState(
      settings.value("APBinMessagesDialog/params/state").toByteArray());
  table_messages->horizontalHeader()->restoreState(
      settings.value("APBinMessagesDialog/messages/state").toByteArray());

  table_params->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
  table_params->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);

  table_messages->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
  table_messages->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
}

APBinMessagesDialog::~APBinMessagesDialog()
{
  QTableWidget* table_params = ui->tableWidgetParams;
  QTableWidget* table_messages = ui->tableWidgetMessages;

  QSettings settings;
  settings.setValue("APBinMessagesDialog/geometry", this->saveGeometry());
  settings.setValue("APBinMessagesDialog/params/state", table_params->horizontalHeader()->saveState());
  settings.setValue("APBinMessagesDialog/messages/state", table_messages->horizontalHeader()->saveState());

  delete ui;
}
