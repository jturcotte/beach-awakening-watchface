module.exports = [
  {
    type: 'section',
    items: [
      {
        type: 'select',
        messageKey: 'ANIMATION_LOOP_DELAY',
        label: 'Animation loop delay',
        description: 'Use a longer delay for better battery life',
        defaultValue: '5000',
        options: [
          {
            label: 'Continuous',
            value: '0'
          },
          {
            label: '2s',
            value: '2000'
          },
          {
            label: '5s',
            value: '5000'
          },
          {
            label: '10s',
            value: '10000'
          },
          {
            label: '15s',
            value: '15000'
          },
          {
            label: '30s',
            value: '30000'
          },
          {
            label: '60s',
            value: '60000'
          }
        ]
      },
      {
        type: 'toggle',
        messageKey: 'SHOW_DATE',
        label: 'Show date',
        defaultValue: true
      },
      {
        type: 'submit',
        defaultValue: 'Save'
      }
    ]
  }
];