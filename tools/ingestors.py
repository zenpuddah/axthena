"""Source adapters. Transport, persistence and graph writes do not belong here."""
import calendar
import datetime as dt
import html
import re
from urllib.parse import quote, urlsplit


def windows(plan):
    start, end = dt.date.fromisoformat(plan['from']), dt.date.fromisoformat(plan['to'])
    while start <= end:
        stop = min(end, start.replace(day=calendar.monthrange(start.year, start.month)[1])) if plan['partition'] == 'month' else end
        yield start, stop
        start = stop + dt.timedelta(days=1)


class WordPress:
    """The same bounded REST posts protocol as C++ collect_public."""
    name = 'wordpress'

    @staticmethod
    def url(source, start, end, page, size):
        after = (start - dt.timedelta(days=1)).isoformat() + 'T00:00:00'
        before = (end + dt.timedelta(days=1)).isoformat() + 'T23:59:59'
        url = (f"{source['endpoint']}?per_page={size}&page={page}&orderby=date&order=asc"
               f"&after={quote(after, safe='')}&before={quote(before, safe='')}"
               '&_fields=id,date,date_gmt,modified,modified_gmt,link,title,content')
        if source.get('search'):
            url += '&search=' + quote(source['search'], safe='')
        return url

    @staticmethod
    def selected_ids(items, source, start, end):
        result = set()
        for item in items:
            try:
                day = dt.date.fromisoformat(item['date'][:10])
                if not start <= day <= end or type(item['id']) is not int:
                    continue
                link = urlsplit(item['link'])
                if link.scheme != 'https' or link.netloc != urlsplit(source['endpoint']).netloc:
                    continue
                title = html.unescape(re.sub('<[^>]*>', '', item['title']['rendered'])).strip()
                if title and isinstance(item['content']['rendered'], str):
                    result.add(item['id'])
            except (KeyError, ValueError, TypeError):
                continue
        return result


# Add an adapter here only with its source-specific contract and fixture tests.
ADAPTERS = {'wordpress': WordPress()}
