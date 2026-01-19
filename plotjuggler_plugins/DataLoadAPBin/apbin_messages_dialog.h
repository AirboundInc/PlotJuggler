#pragma once

#include <QDialog>
#include <QString>
#include <vector>

namespace Ui
{
class APBinMessagesDialog;
}

struct APBinMessage
{
  uint64_t timestamp_us;
  std::string message;
};

class APBinMessagesDialog : public QDialog
{
  Q_OBJECT

public:
  explicit APBinMessagesDialog(const std::vector<APBinMessage>& messages, QWidget* parent = nullptr);

  void restoreSettings();

  ~APBinMessagesDialog();

private:
  Ui::APBinMessagesDialog* ui;
};
