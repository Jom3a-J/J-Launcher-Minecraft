#include "ChooseProviderDialog.h"
#include "ui_ChooseProviderDialog.h"

#include <QPushButton>
#include <QRadioButton>

#include "modplatform/ModIndex.h"
#include "Application.h"

ChooseProviderDialog::ChooseProviderDialog(QWidget* parent, bool single_choice, bool allow_skipping)
    : QDialog(parent), ui(new Ui::ChooseProviderDialog)
{
    ui->setupUi(this);

    addProviders();
    m_providers.button(0)->click();

    connect(ui->skipOneButton, &QPushButton::clicked, this, &ChooseProviderDialog::skipOne);
    connect(ui->skipAllButton, &QPushButton::clicked, this, &ChooseProviderDialog::skipAll);

    connect(ui->confirmOneButton, &QPushButton::clicked, this, &ChooseProviderDialog::confirmOne);
    connect(ui->confirmAllButton, &QPushButton::clicked, this, &ChooseProviderDialog::confirmAll);

    if (single_choice) {
        ui->providersLayout->removeWidget(ui->skipAllButton);
        ui->providersLayout->removeWidget(ui->confirmAllButton);
    }

    if (!allow_skipping) {
        ui->providersLayout->removeWidget(ui->skipOneButton);
        ui->providersLayout->removeWidget(ui->skipAllButton);
    }
}

ChooseProviderDialog::~ChooseProviderDialog()
{
    delete ui;
}

void ChooseProviderDialog::setDescription(QString desc)
{
    ui->explanationLabel->setText(desc);
}

void ChooseProviderDialog::skipOne()
{
    reject();
}
void ChooseProviderDialog::skipAll()
{
    m_response.skip_all = true;
    reject();
}

void ChooseProviderDialog::confirmOne()
{
    m_response.chosen = getSelectedProvider();
    m_response.try_others = ui->tryOthersCheckbox->isChecked();
    accept();
}
void ChooseProviderDialog::confirmAll()
{
    m_response.chosen = getSelectedProvider();
    m_response.confirm_all = true;
    m_response.try_others = ui->tryOthersCheckbox->isChecked();
    accept();
}

auto ChooseProviderDialog::getSelectedProvider() const -> ModPlatform::ResourceProvider
{
    return ModPlatform::ResourceProvider(m_providers.checkedId());
}

void ChooseProviderDialog::addProviders()
{
    QRadioButton* btn;

    for (auto& provider : { ModPlatform::ResourceProvider::MODRINTH, ModPlatform::ResourceProvider::FLAME }) {
        if (provider == ModPlatform::ResourceProvider::FLAME
            && !(APPLICATION->capabilities() & Application::SupportsFlame)) {
            continue;
        }
        btn = new QRadioButton(ModPlatform::ProviderCapabilities::readableName(provider), this);
        m_providers.addButton(btn, static_cast<int>(provider));
        ui->providersLayout->addWidget(btn);
    }
}

void ChooseProviderDialog::disableInput()
{
    for (auto& btn : m_providers.buttons())
        btn->setEnabled(false);

    ui->skipOneButton->setEnabled(false);
    ui->skipAllButton->setEnabled(false);
    ui->confirmOneButton->setEnabled(false);
    ui->confirmAllButton->setEnabled(false);
}
