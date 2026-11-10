"""The security fixes that Chrome's release notes list, from the feed of the Chrome Releases blog.

Each stable update of Chrome lists its security fixes on a line of its own, such as
"[TBD][572055997] High CVE-2026-76029: Improper input validation in Network. Reported by ...", and says so when Google
knows of an exploit in the wild. The bug stays restricted for a while after the release, but its number is public, and
so is the commit that fixes it, which names it on a Bug: line. See docs/releasing.md.
"""

import dataclasses
import html
import re
import urllib.parse
import xml.etree.ElementTree as ElementTree
from dataclasses import dataclass

from tools.release.web import Fetch, WebError

__all__ = ["FEED_URL", "SEVERITIES", "SecurityFix", "security_fixes"]

FEED_URL = "https://chromereleases.googleblog.com/feeds/posts/default/-/Stable%20updates"
"""The Atom feed of the blog's posts on stable updates, for desktop, Android and iOS, newest first."""

SEVERITIES = ("Low", "Medium", "High", "Critical")
"""The ratings that the notes give a fix, from the least to the most severe."""

# About two months of posts, which covers the patch builds of the Supported Window.
_POSTS = 60

_ATOM = "{http://www.w3.org/2005/Atom}"
_TAG = re.compile(r"<[^>]*>")
_FIX = re.compile(
    rf"\[[^\]\[]*\]\s*\[\s*(\d+)\s*\]\s*({'|'.join(SEVERITIES)})\s+(CVE-\d{{4}}-\d+)\s*:\s*([^\]\[]+?)\.\s+Reported by"
)
# As in "Google is aware that an exploit for CVE-2026-125350 exists in the wild.", whose CVEs are those between the
# exploit and the wild.
_IN_THE_WILD = re.compile(r"\bexploits?\b([^.]*?)\bexists?\b[^.]*?\bin the wild\b")
_CVE = re.compile(r"CVE-\d{4}-\d+")


@dataclass(frozen=True, slots=True)
class SecurityFix:
    """A security fix that the release notes list.

    Attributes:
        bug: The number of its bug, which the commits that fix it name.
        severity: How severe the notes rate it, one of SEVERITIES.
        cve: Its CVE, such as "CVE-2026-76029".
        title: What the notes call it, such as "Improper input validation in Network". The part after "in" is the
            component that the bug was filed under, which need not be where the fix is.
        published: The day of the post that lists it, such as "2026-10-29".
        in_the_wild: Whether a post says that Google knows of an exploit for it in the wild.
    """

    bug: str
    severity: str
    cve: str
    title: str
    published: str
    in_the_wild: bool


def security_fixes(fetch: Fetch) -> dict[str, SecurityFix]:
    """Return the security fixes that the newest posts on stable updates list, by bug number.

    A fix that two posts list, such as one in a patch build of two milestones, counts from the older post, which is
    when it first shipped.

    Raises:
        WebError: The blog did not answer, or its answer is not a feed of posts.
    """
    url = f"{FEED_URL}?{urllib.parse.urlencode({'max-results': _POSTS})}"
    try:
        feed = ElementTree.fromstring(fetch(url))
    except ElementTree.ParseError as error:
        raise WebError(f"{url} did not give a feed: {error}") from error
    entries = feed.findall(f"{_ATOM}entry")
    if feed.tag != f"{_ATOM}feed" or not entries:
        raise WebError(f"{url} gave no posts")
    fixes: dict[str, SecurityFix] = {}
    in_the_wild: set[str] = set()
    for entry in entries:
        published, content = entry.findtext(f"{_ATOM}published"), entry.findtext(f"{_ATOM}content")
        if published is None or content is None:
            raise WebError(f"{url} gave a post without its date or text")
        text = " ".join(html.unescape(_TAG.sub(" ", content)).split())
        in_the_wild |= {cve for named in _IN_THE_WILD.findall(text) for cve in _CVE.findall(named)}
        for bug, severity, cve, title in _FIX.findall(text):
            fixes[bug] = SecurityFix(bug, severity, cve, title, published[:10], in_the_wild=False)
    return {bug: dataclasses.replace(fix, in_the_wild=fix.cve in in_the_wild) for bug, fix in fixes.items()}
