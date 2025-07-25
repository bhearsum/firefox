import pytest
from webdriver.bidi.error import UnknownErrorException

URL = "https://musicstore.auone.jp/"
HERO_CSS = ".top-campaignbnr-list__item-link"
UNSUPPORTED_TEXT = "このブラウザはサポートされていません"
NEED_VPN_TEXT = "access from within Japan"


async def is_unsupported(client):
    vpn = False
    try:
        await client.navigate(URL, wait="none", no_skip=True)
        _, vpn = client.await_first_element_of(
            [client.css(HERO_CSS), client.text(NEED_VPN_TEXT)], is_displayed=True
        )
    except UnknownErrorException as e:
        if "NS_ERROR_UNKNOWN_HOST" not in str(e):
            raise e
        vpn = True
    if vpn:
        pytest.skip("Region-locked, cannot test. Try using a VPN set to Japan.")
        return False
    return client.find_text(UNSUPPORTED_TEXT, is_displayed=True)


@pytest.mark.asyncio
@pytest.mark.with_interventions
async def test_enabled(client):
    assert not await is_unsupported(client)


@pytest.mark.asyncio
@pytest.mark.without_interventions
async def test_disabled(client):
    assert await is_unsupported(client)
