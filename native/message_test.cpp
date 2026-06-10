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
}
