#ifndef DB_H
#define DB_H

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QCoreApplication>
#include <QUuid>
#include <QFile>
#include <QRandomGenerator>

extern QSqlDatabase db;

bool initialize_database() {
    db = QSqlDatabase::addDatabase("QSQLITE");
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

    qDebug() << "New database detected! Running initial setup script...";

    // 1. Read and parse schema.sql
    QFile file(QCoreApplication::applicationDirPath()+"/sql/schema.sql");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "Failed to locate schema.sql file at:" << QCoreApplication::applicationDirPath()+"/sql/schema.sql";
        return false;
    }

    QString script = QString::fromUtf8(file.readAll());
    QStringList queries = script.split(';', Qt::SkipEmptyParts);
    file.close();

    db.transaction(); // Wrap in transaction for blazing performance

    for (QString &raw_query: queries) {
        QString query_str = raw_query.trimmed();
        if (query_str.isEmpty()) continue;

        QSqlQuery query(db);
        query.exec("PRAGMA foreign_keys = ON");
        if (!query.exec(query_str)) {
            qDebug() << "SQL Execution Error:" << query.lastError().text();
            qDebug() << "Problem Query:" << query_str;
            db.rollback();
            return false;
        }
    }

    QString setup_key = QUuid::createUuid()
            .toString(QUuid::WithoutBraces)
            .left(QRandomGenerator::global()->bounded(20, 30))
            .toUpper();

    QSqlQuery settings_query;
    settings_query.prepare("INSERT INTO server_settings (key, value) VALUES ('initial_admin_token', :token);"); //
    settings_query.bindValue(":token", setup_key);
    settings_query.exec();

    if (!db.commit()) {
        qDebug() << "Failed to commit schema:"
                 << db.lastError().text();
        return false;
    }

    qDebug() << "\n====================================================================";
    qDebug() << " SERVER STORAGE SYSTEM INITIALIZED SUCCESSFULLY!";
    qDebug() << " No administrative account exists yet.";
    qDebug() << " Use this temporary privilege key inside your client app to claim admin power:";
    qDebug() << " ---> " << setup_key << " <---";
    qDebug() << "====================================================================\n";

    return true;
}

QVariant db_scalar(QSqlDatabase &db, const QString &sql, const QVariantList &args = {}){
    QSqlQuery query(db);
    query.prepare(sql);

    for (const QVariant &arg : args)
        query.addBindValue(arg);

    if (!query.exec()){
        qDebug() << "DB error:" << query.lastError().text();
        return {};
    }

    if (!query.next())
        return {};

    return query.value(0);
}

bool db_update(QSqlDatabase &db, const QString &sql, const QVariantList &args = {}){
    QSqlQuery query(db);
    query.prepare(sql);

    for (const QVariant &arg : args) query.addBindValue(arg);

    if (!query.exec()) {
        qDebug() << "DB error:" << query.lastError().text();
        return false;
    }

    return query.numRowsAffected() > 0;
}

#endif // DB_H
