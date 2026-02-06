#pragma once

#include <QDialog>
#include <QString>
#include <vector>
#include <map>

namespace Ui
{
class APBinMessagesDialog;
}

struct APBinMessage
{
  uint64_t timestamp_us;
  std::string message;
};

struct APBinParameter
{
  std::string name;
  float value;
};

class APBinMessagesDialog : public QDialog
{
  Q_OBJECT

public:
  explicit APBinMessagesDialog(const std::vector<APBinMessage>& messages,
                               const std::map<std::string, float>& parameters,
                               QWidget* parent = nullptr);

  void restoreSettings();

  ~APBinMessagesDialog();

private:
  Ui::APBinMessagesDialog* ui;
};
