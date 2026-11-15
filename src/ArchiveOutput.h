#pragma once
#include <QByteArray>

// Ограниченный хвост для ошибок/процентов и потоковый счётчик строк bsdtar.
// Граница read() может проходить посередине «a имя», в том числе после «a».
class ArchiveOutput
{
public:
    static constexpr int tailLimit = 8192;
    void append(const QByteArray &chunk)
    {
        for (char c : chunk) {
            if (c == '\n') m_prefix = 0;
            else if (m_prefix == 0) m_prefix = c == 'a' ? 1 : 2;
            else if (m_prefix == 1) { if (c == ' ') ++m_added; m_prefix = 2; }
        }
        if (chunk.size() >= tailLimit) m_tail = chunk.right(tailLimit);
        else {
            if (m_tail.size() + chunk.size() > tailLimit)
                m_tail.remove(0, m_tail.size() + chunk.size() - tailLimit);
            m_tail += chunk;
        }
    }
    const QByteArray &tail() const { return m_tail; }
    qsizetype added() const { return m_added; }
private:
    QByteArray m_tail;
    qsizetype m_added = 0;
    int m_prefix = 0;
};
