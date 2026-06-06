var Clay = require('@rebble/clay');
var messageKeys = require('message_keys');
var clayConfig = require('./config');

var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

Pebble.addEventListener('showConfiguration', function() {
	Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function(e) {
	if (!e || !e.response) {
		return;
	}

	var settings = clay.getSettings(e.response);
	var delay = parseInt(settings[messageKeys.ANIMATION_LOOP_DELAY], 10);

	settings[messageKeys.ANIMATION_LOOP_DELAY] = isNaN(delay) ? 5000 : delay;

	Pebble.sendAppMessage(settings, function() {
		console.log('Sent config data to Pebble');
	}, function(error) {
		console.log('Failed to send config data!');
		console.log(JSON.stringify(error));
	});
});