from hermes_gadget.sim.runner import BOARDS


def test_412_round_simulator_profile_matches_the_watcher_screen():
    board = BOARDS["sim-412x412-round"]
    assert (board.width, board.height) == (412, 412)
    assert board.round
    assert board.touch
    assert board.mic
    assert board.speaker
    assert not board.scroll_buttons
