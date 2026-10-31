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

inline void run_database_migrations() {
    QSqlQuery q(db);
    q.exec("PRAGMA foreign_keys = ON;");

    // 1. Ensure permission & role tables exist
    q.exec("CREATE TABLE IF NOT EXISTS permissions ("
           "id INTEGER PRIMARY KEY AUTOINCREMENT, "
           "name TEXT UNIQUE NOT NULL);");

    q.exec("CREATE TABLE IF NOT EXISTS roles ("
           "id INTEGER PRIMARY KEY AUTOINCREMENT, "
           "name TEXT UNIQUE NOT NULL);");

    q.exec("CREATE TABLE IF NOT EXISTS role_permissions ("
           "role_id INTEGER, "
           "permission_id INTEGER, "
           "value INTEGER NOT NULL, "
           "FOREIGN KEY(role_id) REFERENCES roles(id) ON DELETE CASCADE, "
           "FOREIGN KEY(permission_id) REFERENCES permissions(id) ON DELETE CASCADE, "
           "PRIMARY KEY(role_id, permission_id));");

    q.exec("CREATE TABLE IF NOT EXISTS user_roles ("
           "user_id INTEGER, "
           "role_id INTEGER, "
           "FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE, "
           "FOREIGN KEY(role_id) REFERENCES roles(id) ON DELETE CASCADE, "
           "PRIMARY KEY(user_id, role_id));");

    // 2. Check and add columns to rooms table
    q.exec("PRAGMA table_info(rooms);");
    QSet<QString> room_cols;
    while (q.next()) {
        room_cols.insert(q.value(1).toString());
    }

    if (!room_cols.contains("needed_voice_join_power")) {
        q.exec("ALTER TABLE rooms ADD COLUMN needed_voice_join_power INTEGER DEFAULT 0;");
        qDebug() << "Migration: added needed_voice_join_power to rooms";
    }
    if (!room_cols.contains("needed_chat_join_power")) {
        q.exec("ALTER TABLE rooms ADD COLUMN needed_chat_join_power INTEGER DEFAULT 0;");
        qDebug() << "Migration: added needed_chat_join_power to rooms";
    }
    if (!room_cols.contains("needed_speak_power")) {
        q.exec("ALTER TABLE rooms ADD COLUMN needed_speak_power INTEGER DEFAULT 0;");
        qDebug() << "Migration: added needed_speak_power to rooms";
    }

    // 3. Upsert permissions
    q.exec("INSERT INTO permissions (id, name) VALUES "
           "(1, 'u_can_create_rooms'), "
           "(2, 'u_can_delete_rooms'), "
           "(3, 'u_can_ban_users'), "
           "(4, 'u_can_kick_users'), "
           "(5, 'u_voice_join_power'), "
           "(6, 'u_chat_join_power'), "
           "(7, 'u_speak_power') "
           "ON CONFLICT(id) DO UPDATE SET name = excluded.name;");

    // 4. Insert default roles if none exist
    q.exec("SELECT COUNT(*) FROM roles;");
    int role_count = q.next() ? q.value(0).toInt() : 0;
    if (role_count == 0) {
        q.exec("INSERT INTO roles (id, name) VALUES (1, 'Guest');");
        q.exec("INSERT INTO roles (id, name) VALUES (2, 'Member');");
        q.exec("INSERT INTO roles (id, name) VALUES (3, 'Admin');");

        // Guest permissions (power = 0)
        for (int p = 1; p <= 7; ++p) {
            q.exec(QString("INSERT INTO role_permissions (role_id, permission_id, value) VALUES (1, %1, 0);").arg(p));
        }

        // Member permissions (can create rooms, join voice/chat power = 25, speak power = 25)
        q.exec("INSERT INTO role_permissions (role_id, permission_id, value) VALUES (2, 1, 1);"); // u_can_create_rooms
        q.exec("INSERT INTO role_permissions (role_id, permission_id, value) VALUES (2, 2, 0);");
        q.exec("INSERT INTO role_permissions (role_id, permission_id, value) VALUES (2, 3, 0);");
        q.exec("INSERT INTO role_permissions (role_id, permission_id, value) VALUES (2, 4, 0);");
        q.exec("INSERT INTO role_permissions (role_id, permission_id, value) VALUES (2, 5, 25);"); // u_voice_join_power
        q.exec("INSERT INTO role_permissions (role_id, permission_id, value) VALUES (2, 6, 25);"); // u_chat_join_power
        q.exec("INSERT INTO role_permissions (role_id, permission_id, value) VALUES (2, 7, 25);"); // u_speak_power

        // Admin permissions (all = 100 / 1)
        q.exec("INSERT INTO role_permissions (role_id, permission_id, value) VALUES (3, 1, 1);");
        q.exec("INSERT INTO role_permissions (role_id, permission_id, value) VALUES (3, 2, 1);");
        q.exec("INSERT INTO role_permissions (role_id, permission_id, value) VALUES (3, 3, 1);");
        q.exec("INSERT INTO role_permissions (role_id, permission_id, value) VALUES (3, 4, 1);");
        q.exec("INSERT INTO role_permissions (role_id, permission_id, value) VALUES (3, 5, 100);");
        q.exec("INSERT INTO role_permissions (role_id, permission_id, value) VALUES (3, 6, 100);");
        q.exec("INSERT INTO role_permissions (role_id, permission_id, value) VALUES (3, 7, 100);");

        qDebug() << "Migration: created default roles (Guest, Member, Admin)";
    }

    qDebug() << "Database migrations completed.";
}

bool initialize_database() {
    db = QSqlDatabase::addDatabase("QSQLITE");
    db.setDatabaseName(QCoreApplication::applicationDirPath()+"/app.db");
    if (!db.open()) {
        // qDebug() << "Database opening error:" << db.lastError().text();
        return false;
    }

    {
        QSqlQuery pragma(db);
        pragma.exec("PRAGMA journal_mode = WAL;");
        pragma.exec("PRAGMA synchronous = NORMAL;");
    }

    // Check if database is already initialized
    QSqlQuery checkQuery("SELECT name FROM sqlite_master WHERE type='table' AND name='users';");
    if (checkQuery.next()) {
        qDebug() << "Database already exists. Running migrations...";
        QSqlQuery init_pwd(db);
        init_pwd.exec("INSERT OR IGNORE INTO server_settings (key, value) VALUES ('server_password', '');");
        init_pwd.exec("INSERT OR IGNORE INTO server_settings (key, value) VALUES ('server_name', 'Echo Server');");
        run_database_migrations();
        return true;
    }

    qDebug() << "New database detected! Running initial setup script...";

    // 1. Read and parse schema.sql
    QFile file(QCoreApplication::applicationDirPath()+"/sql/schema.sql");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // qDebug() << "Failed to locate schema.sql file at:" << QCoreApplication::applicationDirPath()+"/sql/schema.sql";
        return false;
    }

    QString script = QString::fromUtf8(file.readAll());
    QStringList queries = script.split(';', Qt::SkipEmptyParts);
    file.close();

    db.transaction();

    for (QString &raw_query: queries) {
        QString query_str = raw_query.trimmed();
        if (query_str.isEmpty()) continue;

        QSqlQuery query(db);
        query.exec("PRAGMA foreign_keys = ON");
        if (!query.exec(query_str)) {
            // qDebug() << "SQL Execution Error:" << query.lastError().text();
            // qDebug() << "Problem Query:" << query_str;
            db.rollback();
            return false;
        }
    }

    QString setup_key = QUuid::createUuid()
            .toString(QUuid::WithoutBraces)
            .left(QRandomGenerator::global()->bounded(20, 30))
            .toUpper();

    QSqlQuery settings_query;
    settings_query.prepare("INSERT INTO server_settings (key, value) VALUES ('initial_admin_token', :token);");
    settings_query.bindValue(":token", setup_key);
    settings_query.exec();

    settings_query.exec("INSERT OR IGNORE INTO server_settings (key, value) VALUES ('server_password', '');");
    settings_query.exec("INSERT OR IGNORE INTO server_settings (key, value) VALUES ('server_name', 'Echo Server');");

    if (!db.commit()) {
        // qDebug() << "Failed to commit schema:"
                 // << db.lastError().text();
        return false;
    }

    run_database_migrations();

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
        // qDebug() << "DB error:" << query.lastError().text();
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
        // qDebug() << "DB error:" << query.lastError().text();
        return false;
    }

    return true;
}

#endif // DB_H
