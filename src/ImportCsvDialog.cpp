#include "ImportCsvDialog.h"
#include "ui_ImportCsvDialog.h"
#include "sqlitedb.h"
#include "csvparser.h"

#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QDateTime>
#include <QTextCodec>
#include <QCompleter>
#include <QComboBox>
#include <sqlite3.h>
#include <QFile>
#include <QTextStream>
#include <QSettings>
#include <memory>

ImportCsvDialog::ImportCsvDialog(const QString& filename, DBBrowserDB* db, QWidget* parent)
    : QDialog(parent),
      ui(new Ui::ImportCsvDialog),
      csvFilename(filename),
      pdb(db)
{
    ui->setupUi(this);

    QStringList encodingList;
    foreach(QString enc, QTextCodec::availableCodecs())
        encodingList.push_back(enc);
    encodingCompleter = new QCompleter(encodingList, this);
    encodingCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    ui->editCustomEncoding->setCompleter(encodingCompleter);

    QSettings settings(QApplication::organizationName(), QApplication::organizationName());
    ui->checkboxHeader->setChecked(settings.value("importcsv/firstrowheader", false).toBool());
    ui->checkBoxTrimFields->setChecked(settings.value("importcsv/trimfields", true).toBool());
    setSeparatorChar(QChar(settings.value("importcsv/separator", 44).toInt()));
    setQuoteChar(QChar(settings.value("importcsv/quotecharacter", 34).toInt()));
    setEncoding(settings.value("importcsv/encoding", "UTF-8").toString());

    checkInput();
    updatePreview();
}

ImportCsvDialog::~ImportCsvDialog()
{
    delete ui;
}

namespace {
void rollback(ImportCsvDialog* dialog, DBBrowserDB* pdb, QProgressDialog& progress, const QString& savepointName)
{
    progress.hide();
    QApplication::restoreOverrideCursor();  // restore original cursor
    QString error = QObject::tr("Error importing data. Message from database engine: %1").arg(pdb->lastErrorMessage);
    QMessageBox::warning(dialog, QApplication::applicationName(), error);
    pdb->revert(savepointName);
}
}

class CSVImportProgress : public CSVProgress
{
public:
    CSVImportProgress(size_t filesize)
    {
        m_pProgressDlg = new QProgressDialog(
                    QObject::tr("Decoding CSV file..."),
                    QObject::tr("Cancel"),
                    0,
                    filesize);
        m_pProgressDlg->setWindowModality(Qt::ApplicationModal);
    }

    ~CSVImportProgress()
    {
        delete m_pProgressDlg;
    }

    void start()
    {
        m_pProgressDlg->show();
    }

    bool update(size_t pos)
    {
        m_pProgressDlg->setValue(pos);
        qApp->processEvents();

        return !m_pProgressDlg->wasCanceled();
    }

    void end()
    {
        m_pProgressDlg->hide();
    }

private:
    QProgressDialog* m_pProgressDlg;
};

void ImportCsvDialog::accept()
{
    // save settings
    QSettings settings(QApplication::organizationName(), QApplication::organizationName());
    settings.beginGroup("importcsv");
    settings.setValue("firstrowheader", ui->checkboxHeader->isChecked());
    settings.setValue("separator", currentSeparatorChar());
    settings.setValue("quotecharacter", currentQuoteChar());
    settings.setValue("trimfields", ui->checkBoxTrimFields->isChecked());
    settings.setValue("encoding", currentEncoding());
    settings.endGroup();

    // Parse all csv data
    QFile file(csvFilename);
    file.open(QIODevice::ReadOnly | QIODevice::Text);

    CSVParser csv(ui->checkBoxTrimFields->isChecked(), currentSeparatorChar(), currentQuoteChar());
    csv.setCSVProgress(new CSVImportProgress(file.size()));

    QTextStream tstream(&file);
    tstream.setCodec(currentEncoding().toUtf8());
    csv.parse(tstream);
    file.close();

    if(csv.csv().size() == 0)
        return;

    // Generate field names. These are either taken from the first CSV row or are generated in the format of "fieldXY" depending on the user input
    sqlb::FieldVector fieldList;
    CSVParser::TCSVResult::const_iterator itBegin = csv.csv().begin();
    if(ui->checkboxHeader->isChecked())
    {
        ++itBegin;
        for(QStringList::const_iterator it = csv.csv().at(0).begin();
            it != csv.csv().at(0).end();
            ++it)
        {
            // Remove invalid characters
            QString thisfield = *it;
            thisfield.replace("`", "");
            thisfield.replace(" ", "");
            thisfield.replace('"', "");
            thisfield.replace("'","");
            thisfield.replace(",","");
            thisfield.replace(";","");

            // Avoid empty field names
            if(thisfield.isEmpty())
                thisfield = QString("field%1").arg(std::distance(csv.csv().at(0).begin(), it) + 1);

            fieldList.push_back(sqlb::FieldPtr(new sqlb::Field(thisfield, "")));
        }
    } else {
        for(size_t i=0; i < csv.columns(); ++i)
            fieldList.push_back(sqlb::FieldPtr(new sqlb::Field(QString("field%1").arg(i+1), "")));
    }

    // Show progress dialog
    QProgressDialog progress(tr("Inserting data..."), tr("Cancel"), 0, csv.csv().size());
    progress.setWindowModality(Qt::ApplicationModal);

    // Are we importing into an existing table?
    bool importToExistingTable = false;
    objectMap objects = pdb->getBrowsableObjects();
    for(objectMap::ConstIterator i=objects.begin();i!=objects.end();++i)
    {
        if(i.value().gettype() == "table" && i.value().getname() == ui->editName->text())
        {
            if((size_t)i.value().table.fields().size() != csv.columns())
            {
                QMessageBox::warning(this, QApplication::applicationName(),
                                     tr("There is already a table of that name and an import into an existing table is only possible if the number of columns match."));
                return;
            } else {
                if(QMessageBox::question(this, QApplication::applicationName(), tr("There is already a table of that name. Do you want to import the data into it?"), QMessageBox::Yes, QMessageBox::No) == QMessageBox::Yes)
                {
                    importToExistingTable = true;
                    break;
                } else {
                    return;
                }
            }
        }
    }

    // Create a savepoint, so we can rollback in case of any errors during importing
    // db needs to be saved or an error will occur
    QString restorepointName = QString("CSVIMPORT_%1").arg(QDateTime::currentMSecsSinceEpoch());
    if(!pdb->setRestorePoint(restorepointName))
        return rollback(this, pdb, progress, restorepointName);

    // Create table
    if(!importToExistingTable)
    {
        if(!pdb->createTable(ui->editName->text(), fieldList))
            return rollback(this, pdb, progress, restorepointName);
    }

    // now lets import all data, one row at a time
    for(CSVParser::TCSVResult::const_iterator it = itBegin;
        it != csv.csv().end();
        ++it)
    {
        QString sql;
        sql = QString("INSERT INTO `%1` VALUES(").arg(ui->editName->text());

        for(QStringList::const_iterator jt = it->begin(); jt != it->end(); ++jt)
        {
            // need to mprintf here
            char* formSQL = sqlite3_mprintf("%Q", (const char*)jt->toUtf8());
            sql.append(formSQL);
            if(formSQL)
                sqlite3_free(formSQL);

            if(jt != (it->end() - 1))
                sql.append((','));
        }

        sql.append(");");

        if(!pdb->executeSQL(sql, false, false))
            return rollback(this, pdb, progress, restorepointName);

        progress.setValue(std::distance(csv.csv().begin(), it));
        if(progress.wasCanceled())
            return rollback(this, pdb, progress, restorepointName);
    }

    QApplication::restoreOverrideCursor();  // restore original cursor
    QDialog::accept();
}

void ImportCsvDialog::updatePreview()
{
    // Show/hide custom quote/separator input fields
    ui->editCustomQuote->setVisible(ui->comboQuote->currentIndex() == ui->comboQuote->count()-1);
    ui->editCustomSeparator->setVisible(ui->comboSeparator->currentIndex() == ui->comboSeparator->count()-1);
    ui->editCustomEncoding->setVisible(ui->comboEncoding->currentIndex() == ui->comboEncoding->count()-1);

    // Get preview data
    QFile file(csvFilename);
    file.open(QIODevice::ReadOnly | QIODevice::Text);

    CSVParser csv(ui->checkBoxTrimFields->isChecked(), currentSeparatorChar(), currentQuoteChar());

    QTextStream tstream(&file);
    tstream.setCodec(currentEncoding().toUtf8());
    csv.parse(tstream, 20);
    file.close();

    // Reset preview widget
    ui->tablePreview->clear();
    ui->tablePreview->setColumnCount(csv.columns());

    // Exit if there are no lines to preview at all
    if(csv.columns() == 0)
        return;

    // Use first row as header if necessary
    CSVParser::TCSVResult::const_iterator itBegin = csv.csv().begin();
    if(ui->checkboxHeader->isChecked())
    {
        ui->tablePreview->setHorizontalHeaderLabels(*itBegin);
        ++itBegin;
    }

    // Fill data section
    ui->tablePreview->setRowCount(std::distance(itBegin, csv.csv().end()));

    for(CSVParser::TCSVResult::const_iterator ct = itBegin;
        ct != csv.csv().end();
        ++ct)
    {
        for(QStringList::const_iterator it = ct->begin(); it != ct->end(); ++it)
        {
            int rowNum = std::distance(itBegin, ct);
            if(it == ct->begin())
            {
                ui->tablePreview->setVerticalHeaderItem(
                            rowNum,
                            new QTableWidgetItem(QString::number(rowNum + 1)));
            }
            ui->tablePreview->setItem(
                        rowNum,
                        std::distance(ct->begin(), it),
                        new QTableWidgetItem(*it));
        }
    }
}

void ImportCsvDialog::checkInput()
{
    bool valid = true;
    if(ui->editName->text().isEmpty() || ui->editName->text().contains("`"))
        valid = false;

    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(valid);
}

void ImportCsvDialog::setQuoteChar(const QChar& c)
{
    QComboBox* combo = ui->comboQuote;
    int index = combo->findText(c);
    if(index == -1)
    {
        combo->setCurrentIndex(combo->count());
        ui->editCustomQuote->setText(c);
    }
    else
    {
        combo->setCurrentIndex(index);
    }
}

char ImportCsvDialog::currentQuoteChar() const
{
    // The last item in the combobox is the 'Other' item; if it is selected return the text of the line edit field instead
    if(ui->comboQuote->currentIndex() == ui->comboQuote->count()-1)
        return ui->editCustomQuote->text().length() ? ui->editCustomQuote->text().at(0).toLatin1() : 0;

    if(ui->comboQuote->currentText().length())
        return ui->comboQuote->currentText().at(0).toLatin1();
    else
        return 0;
}

void ImportCsvDialog::setSeparatorChar(const QChar& c)
{
    QComboBox* combo = ui->comboSeparator;
    QString sText = c == '\t' ? QString("Tab") : QString(c);
    int index = combo->findText(sText);
    if(index == -1)
    {
        combo->setCurrentIndex(combo->count());
        ui->editCustomSeparator->setText(c);
    }
    else
    {
        combo->setCurrentIndex(index);
    }
}

char ImportCsvDialog::currentSeparatorChar() const
{
    // The last item in the combobox is the 'Other' item; if it is selected return the text of the line edit field instead
    if(ui->comboSeparator->currentIndex() == ui->comboSeparator->count()-1)
        return ui->editCustomSeparator->text().length() ? ui->editCustomSeparator->text().at(0).toLatin1() : 0;

    return ui->comboSeparator->currentText() == tr("Tab") ? '\t' : ui->comboSeparator->currentText().at(0).toLatin1();
}

void ImportCsvDialog::setEncoding(const QString& sEnc)
{
    QComboBox* combo = ui->comboEncoding;
    int index = combo->findText(sEnc);
    if(index == -1)
    {
        combo->setCurrentIndex(combo->count());
        ui->editCustomEncoding->setText(sEnc);
    }
    else
    {
        combo->setCurrentIndex(index);
    }
}

QString ImportCsvDialog::currentEncoding() const
{
    // The last item in the combobox is the 'Other' item; if it is selected return the text of the line edit field instead
    if(ui->comboEncoding->currentIndex() == ui->comboEncoding->count()-1)
        return ui->editCustomEncoding->text().length() ? ui->editCustomEncoding->text() : "UTF-8";
    else
        return ui->comboEncoding->currentText();
}