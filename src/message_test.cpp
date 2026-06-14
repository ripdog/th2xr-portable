#include "message.hpp"

int main()
{
    th2::Message message;
    message.set("one\\k two\nthree\\kfour");
    if (message.visible() != "one") {
        return 1;
    }
    if (!message.reveal_next() || message.visible() != "one two\nthree") {
        return 2;
    }
    if (!message.reveal_next() || message.visible() != "one two\nthreefour"
        || message.reveal_next()) {
        return 3;
    }

    message.set("first");
    message.append("\nsecond\\kthird");
    if (message.visible() != "first\nsecond"
        || !message.reveal_next() || message.visible() != "first\nsecondthird") {
        return 4;
    }

    message.set("...n...nn\\k W...hat?\\k It's already morning?...");
    if (message.visible() != "...n...nn"
        || !message.reveal_next()
        || message.visible() != "...n...nn W...hat?"
        || !message.reveal_next()
        || message.visible() != "...n...nn W...hat? It's already morning?...") {
        return 5;
    }

    message.set("first\\ksecond");
    if (!message.has_hidden_segments()
        || !message.reveal_next()
        || message.has_hidden_segments()
        || message.visible() != "firstsecond") {
        return 6;
    }

    message.set("single checkpoint\\k");
    if (message.visible() != "single checkpoint"
        || message.has_hidden_segments()
        || message.reveal_next()) {
        return 7;
    }

    message.set("<S10>Instant text<S5> normal text<W60>");
    if (message.visible() != "Instant text normal text") {
        return 8;
    }

    message.set("spoken line");
    message.append(" \\k");
    if (message.visible() != "spoken line") {
        return 9;
    }

    message.set("I'm eating an egg sandwich\x01\x80");
    if (message.visible() != "I'm eating an egg sandwich") {
        return 10;
    }

    message.set("Huhuhu\xe2\x99\xaa");
    if (message.visible() != "Huhuhu\xe2\x99\xaa") {
        return 11;
    }
}
