#ifndef DB_H
#define DB_H

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QCoreApplication>
#include <QUuid>
#include <QFile>

bool initialize_database() {
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE");
    db.setDatabaseName(QCoreApplication::applicationDirPath()+"/app.db");

    if (!db.open()) {
        qDebug() << "Database opening error:" << db.lastError().text();
        return false;
    }

    // Check if database is already initialized
    QSqlQuery checkQuery("SELECT name FROM sqlite_master WHERE type='table' AND name='users';");
    if (checkQuery.next()) {
        qDebug() << "Database already exists. Skipping initialization.";
        return true;
    }

    qDebug() << "Brand new database detected! Running initial setup script...";

    // 1. Read and parse schema.sql
    QFile file(QCoreApplication::applicationDirPath()+"/sql/schema.sql");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "Failed to locate schema.sql file at:" << QCoreApplication::applicationDirPath()+"/sql/schema.sql";
        return false;
    }

    // Split statements by semicolon to handle multiple CREATE TABLE instructions cleanly
    QString script = QString::fromUtf8(file.readAll());
    QStringList queries = script.split(';', Qt::SkipEmptyParts);
    file.close();

    db.transaction(); // Wrap in transaction for blazing performance

    for (QString raw_query: queries) {
        QString query_str = raw_query.trimmed();
        if (query_str.isEmpty()) continue;

        QSqlQuery query;
        if (!query.exec(query_str)) {
            qDebug() << "SQL Execution Error:" << query.lastError().text();
            qDebug() << "Problem Query:" << query_str;
            db.rollback();
            return false;
        }
    }

    // 2. Insert Core Hardcoded Permissions (Your application enum rules)
    QStringList static_permissions = {
        "b_can_create_rooms",
        "b_can_delete_rooms",
        "b_can_ban_users",
        "i_channel_join_power",
        "i_channel_speak_power"
    };

    QSqlQuery permQuery;
    permQuery.prepare("INSERT INTO permissions (name) VALUES (:name);"); // Maps into permissions lookup table
    for (const QString& perm : static_permissions) {
        permQuery.bindValue(":name", perm);
        permQuery.exec();
    }

    // 4. Generate & Securely store the First-Boot Setup Key
    QString setupKey = QUuid::createUuid().toString(QUuid::WithoutBraces).left(12).toUpper(); // e.g. "A1B2C3D4E5F6"

    QSqlQuery settingsQuery;
    settingsQuery.prepare("INSERT INTO server_settings (key, value) VALUES ('initial_admin_token', :token);"); //
    settingsQuery.bindValue(":token", setupKey);
    settingsQuery.exec();

    db.commit(); // Save everything cleanly to the SQLite physical disk loop

    // Print out token block clearly to your server binary's terminal console logs
    qDebug() << "\n====================================================================";
    qDebug() << " SERVER STORAGE SYSTEM INITIALIZED SUCCESSFULLY!";
    qDebug() << " No administrative account exists yet.";
    qDebug() << " Use this temporary privilege key inside your client app to claim admin power:";
    qDebug() << " ---> " << setupKey << " <---";
    qDebug() << "====================================================================\n";

    return true;
}

#endif // DB_H
