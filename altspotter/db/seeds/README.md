# Seeds

Справочник бирж (bitget/binance_usdm/okx/bybit), типы рынков, политики
retention и группа core-активов заполняются схемой `db/migrations/0001_schema.sql`.
Инструменты и контракты создаёт компонент `instrument_catalog` из живого API —
статические seed-данные для них намеренно отсутствуют (источник истины —
биржа + whitelist `config/symbols.yaml`).
